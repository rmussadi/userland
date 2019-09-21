/**
 * Command line program to capture a still frame and encode it to file.
 * Also optionally display a preview/viewfinder of current camera input.
 *
 * Description
 *
 * 3 components are created; camera, preview and JPG encoder.
 * Camera component has three ports, preview, video and stills.
 * This program connects preview and stills to the preview and jpg
 * encoder. Using mmal we don't need to worry about buffers between these
 * components, but we do need to handle buffers from the encoder, which
 * are simply written straight to the file in the requisite buffer callback.
 *
 * We use the RaspiCamControl code to handle the specific camera settings.
 */

// We use some GNU extensions (asprintf, basename)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <sysexits.h>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#include "RaspiStill.h"
#include "RaspiCommonSettings.h"
#include "RaspiCamControl.h"
#include "RaspiPreview.h"
#include "RaspiCLI.h"
#include "RaspiTex.h"
#include "RaspiHelpers.h"

#include <semaphore.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

// Standard port setting for the camera component
//#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3


/// Frame advance method
enum
{
   FRAME_NEXT_SINGLE,
   FRAME_NEXT_FOREVER,
};

/// Amount of time before first image taken to allow settling of
/// exposure etc. in milliseconds.
#define CAMERA_SETTLE_TIME       1000

/** Structure containing all state information for the current run
 */
typedef struct
{
   RASPICOMMONSETTINGS_PARAMETERS common_settings;     /// Common settings
   int timeout;                        /// Time taken before frame is grabbed and app then shuts down. Units are milliseconds
   int fullResPreview;                 /// If set, the camera preview port runs at capture resolution. Reduces fps.
   int frameNextMethod;                /// Which method to use to advance to next frame

   RASPIPREVIEW_PARAMETERS preview_parameters;    /// Preview setup parameters
   RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

   MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component

   RASPITEX_STATE raspitex_state; /// GL renderer state and parameters

} RASPISTILL_STATE;



/// Command ID's and Structure defining our command line options
enum
{
   CommandTimeout,
   CommandFullResPreview,
};

static COMMAND_LIST cmdline_commands[] =
{
   { CommandTimeout, "-timeout",    "t",  "Time (in ms) before takes picture and shuts down (if not specified, set to 5s)", 1 },
   { CommandFullResPreview,"-fullpreview","fp", "Run the preview using the still capture resolution (may reduce preview fps)", 0},
};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);



/**
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPISTILL_STATE *state)
{
   memset(state, 0, sizeof(*state));

   raspicommonsettings_set_defaults(&state->common_settings);

   state->timeout = -1; // replaced with 5000ms later if unset
   state->camera_component = NULL;
   state->fullResPreview = 0;
   state->frameNextMethod = FRAME_NEXT_SINGLE;

   // Setup preview window defaults
   raspipreview_set_defaults(&state->preview_parameters);
   // Set up the camera_parameters to default
   raspicamcontrol_set_defaults(&state->camera_parameters);
   // Set initial GL preview state
   raspitex_set_defaults(&state->raspitex_state);
}

/**
 */
static void dump_status(RASPISTILL_STATE *state)
{
   int i;

   if (!state)
   {
      vcos_assert(0);
      return;
   }

   raspicommonsettings_dump_parameters(&state->common_settings);

   fprintf(stderr, "Time delay %d\n", state->timeout);
   fprintf(stderr, "Full resolution preview %s\n", state->fullResPreview ? "Yes": "No");

   raspipreview_dump_parameters(&state->preview_parameters);
   raspicamcontrol_dump_parameters(&state->camera_parameters);
}

/**
 * Display usage information for the application to stdout
 *
 * @param app_name String to display as the application name
 */
static void application_help_message(char *app_name)
{
   fprintf(stdout, "Runs camera for specific time, and take JPG capture at end if requested\n\n");
   fprintf(stdout, "usage: %s [options]\n\n", app_name);
   fprintf(stdout, "Image parameter commands\n\n");
   raspicli_display_help(cmdline_commands, cmdline_commands_size);
   raspitex_display_help();
   return;
}

/**
 * Parse the incoming command line and put resulting parameters in to the state
 *
 * @param argc Number of arguments in command line
 * @param argv Array of pointers to strings from command line
 * @param state Pointer to state structure to assign any discovered parameters to
 * @return non-0 if failed for some reason, 0 otherwise
 */
static int parse_cmdline(int argc, const char **argv, RASPISTILL_STATE *state)
{
   // Parse the command line arguments.
   // We are looking for --<something> or -<abbreviation of something>

   int valid = 1;
   int i;

   for (i = 1; i < argc && valid; i++)
   {
      int command_id, num_parameters;

      if (!argv[i])
         continue;

      if (argv[i][0] != '-')
      {
         valid = 0;
         continue;
      }

      // Assume parameter is valid until proven otherwise
      valid = 1;

      command_id = raspicli_get_command_id(cmdline_commands, cmdline_commands_size, &argv[i][1], &num_parameters);

      // If we found a command but are missing a parameter, continue (and we will drop out of the loop)
      if (command_id != -1 && num_parameters > 0 && (i + 1 >= argc) )
         continue;

      //  We are now dealing with a command line option
      switch (command_id)
      {
      case CommandTimeout: // Time to run viewfinder for before taking picture, in seconds
      {
         if (sscanf(argv[i + 1], "%d", &state->timeout) == 1)
         {
            // Ensure that if previously selected CommandKeypress we don't overwrite it
            if (state->timeout == 0 && state->frameNextMethod == FRAME_NEXT_SINGLE)
               state->frameNextMethod = FRAME_NEXT_FOREVER;

            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandFullResPreview:
         state->fullResPreview = 1;
         break;

      default:
      {
         // Try parsing for any image specific parameters
         // result indicates how many parameters were used up, 0,1,2
         // but we adjust by -1 as we have used one already
         const char *second_arg = (i + 1 < argc) ? argv[i + 1] : NULL;
         int parms_used = raspicamcontrol_parse_cmdline(&state->camera_parameters, &argv[i][1], second_arg);

         // Still unused, try common settings
         if (!parms_used)
            parms_used = raspicommonsettings_parse_cmdline(&state->common_settings, &argv[i][1], second_arg, &application_help_message);

         // Still unused, try preview settings
         if (!parms_used)
            parms_used = raspipreview_parse_cmdline(&state->preview_parameters, &argv[i][1], second_arg);

         // Still unused, try GL preview options
         if (!parms_used)
            parms_used = raspitex_parse_cmdline(&state->raspitex_state, &argv[i][1], second_arg);

         // If no parms were used, this must be a bad parameters
         if (!parms_used)
            valid = 0;
         else
            i += parms_used - 1;

         break;
      }
      }
   }

   /* GL preview parameters use preview parameters as defaults unless overriden */
   if (! state->raspitex_state.gl_win_defined)
   {
      state->raspitex_state.x       = state->preview_parameters.previewWindow.x;
      state->raspitex_state.y       = state->preview_parameters.previewWindow.y;
      state->raspitex_state.width   = state->preview_parameters.previewWindow.width;
      state->raspitex_state.height  = state->preview_parameters.previewWindow.height;
   }
   /* Also pass the preview information through so GL renderer can determine
    * the real resolution of the multi-media image */
   state->raspitex_state.preview_x       = state->preview_parameters.previewWindow.x;
   state->raspitex_state.preview_y       = state->preview_parameters.previewWindow.y;
   state->raspitex_state.preview_width   = state->preview_parameters.previewWindow.width;
   state->raspitex_state.preview_height  = state->preview_parameters.previewWindow.height;
   state->raspitex_state.opacity         = state->preview_parameters.opacity;
   state->raspitex_state.verbose         = state->common_settings.verbose;

   if (!valid)
   {
      fprintf(stderr, "Invalid command line option (%s)\n", argv[i-1]);
      return 1;
   }

   return 0;
}


/**
 * Create the camera component, set up its ports
 * @param state Pointer to state control struct. camera_component member set to the created camera_component if successful.
 * @return MMAL_SUCCESS if all OK, something else otherwise
 */
static MMAL_STATUS_T create_camera_component(RASPISTILL_STATE *state)
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
   MMAL_STATUS_T status;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
   assert(status == MMAL_SUCCESS);
   status = raspicamcontrol_set_stereo_mode(camera->output[0], &state->camera_parameters.stereo_mode);
   status += raspicamcontrol_set_stereo_mode(camera->output[1], &state->camera_parameters.stereo_mode);
   status += raspicamcontrol_set_stereo_mode(camera->output[2], &state->camera_parameters.stereo_mode);
   assert (status == MMAL_SUCCESS);

   MMAL_PARAMETER_INT32_T camera_num =
     {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->common_settings.cameraNum};

   status = mmal_port_parameter_set(camera->control, &camera_num.hdr);
   assert (status == MMAL_SUCCESS);  // vcos_log_error("Could not select camera : error %d", status);

   assert (camera->output_num); //vcos_log_error("Camera doesn't have output ports");
   status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, state->common_settings.sensor_mode);
   assert (status == MMAL_SUCCESS);//      vcos_log_error("Could not set sensor mode : error %d", status);

   preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
   video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
   still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

   // Enable the camera, and tell it its control callback function
   status = mmal_port_enable(camera->control, default_camera_control_callback);
   assert (status == MMAL_SUCCESS); //      vcos_log_error("Unable to enable control port : error %d", status);

   //  set up the camera configuration
   {
      MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
      {
         { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
         .max_stills_w = state->common_settings.width,
         .max_stills_h = state->common_settings.height,
         .stills_yuv422 = 0,
         .one_shot_stills = 1,
         .max_preview_video_w = state->preview_parameters.previewWindow.width,
         .max_preview_video_h = state->preview_parameters.previewWindow.height,
         .num_preview_video_frames = 3,
         .stills_capture_circular_buffer_height = 0,
         .fast_preview_resume = 0,
         .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
      };

      if (state->fullResPreview) {
         cam_config.max_preview_video_w = state->common_settings.width;
         cam_config.max_preview_video_h = state->common_settings.height;
      }
      mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

   // Now set up the port formats
   format = preview_port->format;
   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   if(state->camera_parameters.shutter_speed > 6000000)
   {
      MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
         { 50, 1000 }, {166, 1000}
      };
      mmal_port_parameter_set(preview_port, &fps_range.hdr);
   }
   else if(state->camera_parameters.shutter_speed > 1000000)
   {
      MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
         { 166, 1000 }, {999, 1000}
      };
      mmal_port_parameter_set(preview_port, &fps_range.hdr);
   }
   if (state->fullResPreview)
   {
      // In this mode we are forcing the preview to be generated from the full capture resolution.
      // This runs at a max of 15fps with the OV5647 sensor.
      format->es->video.width = VCOS_ALIGN_UP(state->common_settings.width, 32);
      format->es->video.height = VCOS_ALIGN_UP(state->common_settings.height, 16);
      format->es->video.crop.x = 0;
      format->es->video.crop.y = 0;
      format->es->video.crop.width = state->common_settings.width;
      format->es->video.crop.height = state->common_settings.height;
      format->es->video.frame_rate.num = FULL_RES_PREVIEW_FRAME_RATE_NUM;
      format->es->video.frame_rate.den = FULL_RES_PREVIEW_FRAME_RATE_DEN;
   }
   else
   {
      // Use a full FOV 4:3 mode
      format->es->video.width = VCOS_ALIGN_UP(state->preview_parameters.previewWindow.width, 32);
      format->es->video.height = VCOS_ALIGN_UP(state->preview_parameters.previewWindow.height, 16);
      format->es->video.crop.x = 0;
      format->es->video.crop.y = 0;
      format->es->video.crop.width = state->preview_parameters.previewWindow.width;
      format->es->video.crop.height = state->preview_parameters.previewWindow.height;
      format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
      format->es->video.frame_rate.den = PREVIEW_FRAME_RATE_DEN;
   }

   status = mmal_port_format_commit(preview_port);
   assert(status == MMAL_SUCCESS);

   // Set the same format on the video  port (which we don't use here)
   mmal_format_full_copy(video_port->format, format);
   status = mmal_port_format_commit(video_port);
   assert(status == MMAL_SUCCESS);

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   format = still_port->format;

   if(state->camera_parameters.shutter_speed > 6000000)
   {
      MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
         { 50, 1000 }, {166, 1000}
      };
      mmal_port_parameter_set(still_port, &fps_range.hdr);
   }
   else if(state->camera_parameters.shutter_speed > 1000000)
   {
      MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
         { 167, 1000 }, {999, 1000}
      };
      mmal_port_parameter_set(still_port, &fps_range.hdr);
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   status = mmal_component_enable(camera);
   assert(status == MMAL_SUCCESS);

   status = raspitex_configure_preview_port(&state->raspitex_state, preview_port);
   assert(status == MMAL_SUCCESS);
   state->camera_component = camera;

   if (state->common_settings.verbose)
      fprintf(stderr, "Camera component done\n");

   return status;

error:

   if (camera)
      mmal_component_destroy(camera);

   return status;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPISTILL_STATE *state)
{
   if (state->camera_component)
   {
      mmal_component_destroy(state->camera_component);
      state->camera_component = NULL;
   }
}


/**
 * Function to wait in various ways (depending on settings) for the next frame
 *
 * @param state Pointer to the state data
 * @param [in][out] frame The last frame number, adjusted to next frame number on output
 * @return !0 if to continue, 0 if reached end of run
 */
static int wait_for_next_frame(RASPISTILL_STATE *state, int *frame)
{
   static int64_t complete_time = -1;
   int keep_running = 1;

   int64_t current_time =  get_microseconds64()/1000;

   if (complete_time == -1)
      complete_time =  current_time + state->timeout;

   // if we have run out of time, flag we need to exit
   // If timeout = 0 then always continue
   if (current_time >= complete_time && state->timeout != 0)
      keep_running = 0;

   switch (state->frameNextMethod)
   {
   case FRAME_NEXT_SINGLE :
      vcos_sleep(state->timeout);  // simple timeout for a single capture
      fprintf(stderr, "Sleep done\n");
      return 0;

   case FRAME_NEXT_FOREVER :
      *frame+=1;
      vcos_sleep(10000); // Have a sleep so we don't hog the CPU.
      return 1;  // Run forever so never indicate end of loop
   } // end of switch

   // Should have returned by now, but default to timeout
   return keep_running;
}


// Our main data storage vessel..
RASPISTILL_STATE rstate;
   
MMAL_PORT_T *camera_preview_port = NULL;
MMAL_PORT_T *camera_video_port = NULL;
//MMAL_PORT_T *camera_still_port = NULL;
//MMAL_PORT_T *preview_input_port = NULL;


int rs_init(int argc, const char **argv)
{
   MMAL_STATUS_T status = MMAL_SUCCESS;

   bcm_host_init();
   // Register our application with the logging system
   vcos_log_register("RaspiStill", VCOS_LOG_CATEGORY);
   signal(SIGINT, default_signal_handler);
   // Disable USR1 and USR2 for the moment - may be reenabled if go in to signal capture mode
   signal(SIGUSR1, SIG_IGN);
   signal(SIGUSR2, SIG_IGN);
   set_app_name(argv[0]);

   default_status(&rstate);

   // Do we have any parameters
   if (argc == 1)   {
      display_valid_parameters(basename(argv[0]), &application_help_message);
      exit(EX_USAGE);
   }
   // Parse the command line and put options in to our status structure
   if (parse_cmdline(argc, argv, &rstate))   {
      exit(EX_USAGE);
   }

   if (rstate.timeout == -1)
      rstate.timeout = 5000;

   // Setup for sensor specific parameters
   get_sensor_defaults(rstate.common_settings.cameraNum, rstate.common_settings.camera_name,
                       &rstate.common_settings.width, &rstate.common_settings.height);
   if (rstate.common_settings.verbose)   {
      print_app_details(stderr);
      dump_status(&rstate);
   }

   raspitex_init(&rstate.raspitex_state);
   // OK, we have a nice set of parameters. Now set up our components. We have 3 components. Camera, Preview and encoder.
   // Camera and encoder are different in stills/video, but preview is the same so handed off to a separate module
   assert ((status = create_camera_component(&rstate)) == MMAL_SUCCESS);

   if (rstate.common_settings.verbose)
     fprintf(stderr, "Starting component connection stage\n");

   camera_preview_port = rstate.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
   camera_video_port   = rstate.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
   //   camera_still_port   = rstate.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];

   if(raspitex_start(&rstate.raspitex_state) != 0)
     return -1;
}

void begin_loop()
{
  int frame;
   
   while (wait_for_next_frame(&rstate, &frame)) {
   } // end for (frame)
}

int rs_teardown()
{
  //   mmal_status_to_int(status);

   if (rstate.common_settings.verbose)
     fprintf(stderr, "Closing down\n");
   
   raspitex_stop(&rstate.raspitex_state);
   raspitex_destroy(&rstate.raspitex_state);
   
   // Disable all our ports that are not handled by connections
   check_disable_port(camera_video_port);   
   
   if (rstate.camera_component)
     mmal_component_disable(rstate.camera_component);
   
   raspipreview_destroy(&rstate.preview_parameters);
   destroy_camera_component(&rstate);
   
   if (rstate.common_settings.verbose)
     fprintf(stderr, "Close down completed, all components disconnected, disabled and destroyed\n\n");

   //if (status != MMAL_SUCCESS)
   //  raspicamcontrol_check_configuration(128);
   
   return EX_OK;
}

typedef int (*callback_type)(float, float, void*);

unsigned char buffer[] = {1,2,3,4,5,6,7,8,9,10};
int callmeback(callback_type t)
{
   t(2.0,1.0, buffer);
}

