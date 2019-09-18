/*
*/

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
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Stills format information
// 0 implies variable
#define STILLS_FRAME_RATE_NUM 0
#define STILLS_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3


/// Frame advance method
enum
{
   FRAME_NEXT_SINGLE,
   FRAME_NEXT_TIMELAPSE,
   FRAME_NEXT_KEYPRESS,
   FRAME_NEXT_FOREVER,
   FRAME_NEXT_GPIO,
   FRAME_NEXT_SIGNAL,
   FRAME_NEXT_IMMEDIATELY
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
   int quality;                        /// JPEG quality setting (1-100)
   int wantRAW;                        /// Flag for whether the JPEG metadata also contains the RAW bayer image
   char *linkname;                     /// filename of output file
   int frameStart;                     /// First number of frame output counter
   MMAL_PARAM_THUMBNAIL_CONFIG_T thumbnailConfig;
   int demoMode;                       /// Run app in demo mode
   int demoInterval;                   /// Interval between camera settings changes
   MMAL_FOURCC_T encoding;             /// Encoding to use for the output file.
   int timelapse;                      /// Delay between each picture in timelapse mode. If 0, disable timelapse
   int fullResPreview;                 /// If set, the camera preview port runs at capture resolution. Reduces fps.
   int frameNextMethod;                /// Which method to use to advance to next frame
   int useGL;                          /// Render preview using OpenGL
   int glCapture;                      /// Save the GL frame-buffer instead of camera output
   int burstCaptureMode;               /// Enable burst mode
   int datetime;                       /// Use DateTime instead of frame#
   int timestamp;                      /// Use timestamp instead of frame#
   int restart_interval;               /// JPEG restart interval. 0 for none.

   RASPIPREVIEW_PARAMETERS preview_parameters;    /// Preview setup parameters
   RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

   MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
   MMAL_COMPONENT_T *encoder_component;   /// Pointer to the encoder component
   MMAL_COMPONENT_T *null_sink_component; /// Pointer to the null sink component
   MMAL_CONNECTION_T *preview_connection; /// Pointer to the connection from camera to preview
   MMAL_CONNECTION_T *encoder_connection; /// Pointer to the connection from camera to encoder

   MMAL_POOL_T *encoder_pool; /// Pointer to the pool of buffers used by encoder output port

   RASPITEX_STATE raspitex_state; /// GL renderer state and parameters

} RASPISTILL_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
typedef struct
{
   FILE *file_handle;                   /// File handle to write buffer data to.
   VCOS_SEMAPHORE_T complete_semaphore; /// semaphore which is posted when we reach end of frame (indicates end of capture or fault)
   RASPISTILL_STATE *pstate;            /// pointer to our state in case required in callback
} PORT_USERDATA;


/// Command ID's and Structure defining our command line options
enum
{
   CommandQuality,
   CommandRaw,
   CommandTimeout,
   CommandThumbnail,
   CommandDemoMode,
   CommandEncoding,
   CommandTimelapse,
   CommandFullResPreview,
   CommandLink,
   CommandKeypress,
   CommandSignal,
   CommandGL,
   CommandGLCapture,
   CommandBurstMode,
   CommandDateTime,
   CommandTimeStamp,
   CommandFrameStart,
   CommandRestartInterval,
};

static COMMAND_LIST cmdline_commands[] =
{
   { CommandQuality, "-quality",    "q",  "Set jpeg quality <0 to 100>", 1 },
   { CommandRaw,     "-raw",        "r",  "Add raw bayer data to jpeg metadata", 0 },
   { CommandLink,    "-latest",     "l",  "Link latest complete image to filename <filename>", 1},
   { CommandTimeout, "-timeout",    "t",  "Time (in ms) before takes picture and shuts down (if not specified, set to 5s)", 1 },
   { CommandThumbnail,"-thumb",     "th", "Set thumbnail parameters (x:y:quality) or none", 1},
   { CommandDemoMode,"-demo",       "d",  "Run a demo mode (cycle through range of camera options, no capture)", 0},
   { CommandEncoding,"-encoding",   "e",  "Encoding to use for output file (jpg, bmp, gif, png)", 1},
   { CommandTimelapse,"-timelapse", "tl", "Timelapse mode. Takes a picture every <t>ms. %d == frame number (Try: -o img_%04d.jpg)", 1},
   { CommandFullResPreview,"-fullpreview","fp", "Run the preview using the still capture resolution (may reduce preview fps)", 0},
   { CommandKeypress,"-keypress",   "k",  "Wait between captures for a ENTER, X then ENTER to exit", 0},
   { CommandSignal,  "-signal",     "s",  "Wait between captures for a SIGUSR1 or SIGUSR2 from another process", 0},
   { CommandGL,      "-gl",         "g",  "Draw preview to texture instead of using video render component", 0},
   { CommandGLCapture, "-glcapture","gc", "Capture the GL frame-buffer instead of the camera image", 0},
   { CommandBurstMode, "-burst",    "bm", "Enable 'burst capture mode'", 0},
   { CommandDateTime,  "-datetime",  "dt", "Replace output pattern (%d) with DateTime (MonthDayHourMinSec)", 0},
   { CommandTimeStamp, "-timestamp", "ts", "Replace output pattern (%d) with unix timestamp (seconds since 1970)", 0},
   { CommandFrameStart,"-framestart","fs",  "Starting frame number in output pattern(%d)", 1},
   { CommandRestartInterval, "-restart","rs","JPEG Restart interval (default of 0 for none)", 1},
};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);

static struct
{
   char *format;
   MMAL_FOURCC_T encoding;
} encoding_xref[] =
{
   {"jpg", MMAL_ENCODING_JPEG},
   {"bmp", MMAL_ENCODING_BMP},
   {"gif", MMAL_ENCODING_GIF},
   {"png", MMAL_ENCODING_PNG},
   {"ppm", MMAL_ENCODING_PPM},
   {"tga", MMAL_ENCODING_TGA}
};

static int encoding_xref_size = sizeof(encoding_xref) / sizeof(encoding_xref[0]);


static struct
{
   char *description;
   int nextFrameMethod;
} next_frame_description[] =
{
   {"Single capture",         FRAME_NEXT_SINGLE},
   {"Capture on timelapse",   FRAME_NEXT_TIMELAPSE},
   {"Capture on keypress",    FRAME_NEXT_KEYPRESS},
   {"Run forever",            FRAME_NEXT_FOREVER},
   {"Capture on GPIO",        FRAME_NEXT_GPIO},
   {"Capture on signal",      FRAME_NEXT_SIGNAL},
};

static int next_frame_description_size = sizeof(next_frame_description) / sizeof(next_frame_description[0]);


/**
 * Assign a default set of parameters to the state passed in
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPISTILL_STATE *state)
{
   memset(state, 0, sizeof(*state));

   raspicommonsettings_set_defaults(&state->common_settings);

   state->timeout = -1; // replaced with 5000ms later if unset
   state->quality = 85;
   state->wantRAW = 0;
   state->linkname = NULL;
   state->frameStart = 0;
   state->thumbnailConfig.enable = 1;
   state->thumbnailConfig.width = 64;
   state->thumbnailConfig.height = 48;
   state->thumbnailConfig.quality = 35;
   state->demoMode = 0;
   state->demoInterval = 250; // ms
   state->camera_component = NULL;
   state->encoder_component = NULL;
   state->preview_connection = NULL;
   state->encoder_connection = NULL;
   state->encoder_pool = NULL;
   state->encoding = MMAL_ENCODING_JPEG;
   state->timelapse = 0;
   state->fullResPreview = 0;
   state->frameNextMethod = FRAME_NEXT_SINGLE;
   state->useGL = 0;
   state->glCapture = 0;
   state->burstCaptureMode=0;
   state->datetime = 0;
   state->timestamp = 0;
   state->restart_interval = 0;

   // Setup preview window defaults
   raspipreview_set_defaults(&state->preview_parameters);

   // Set up the camera_parameters to default
   raspicamcontrol_set_defaults(&state->camera_parameters);

   // Set initial GL preview state
   raspitex_set_defaults(&state->raspitex_state);
}

/**
 * Dump image state parameters to stderr. Used for debugging
 *
 * @param state Pointer to state structure to assign defaults to
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

   fprintf(stderr, "Quality %d, Raw %s\n", state->quality, state->wantRAW ? "yes" : "no");
   fprintf(stderr, "Thumbnail enabled %s, width %d, height %d, quality %d\n",
           state->thumbnailConfig.enable ? "Yes":"No", state->thumbnailConfig.width,
           state->thumbnailConfig.height, state->thumbnailConfig.quality);

   fprintf(stderr, "Time delay %d, Timelapse %d\n", state->timeout, state->timelapse);
   fprintf(stderr, "Link to latest frame enabled ");
   if (state->linkname)
   {
      fprintf(stderr, " yes, -> %s\n", state->linkname);
   }
   else
   {
      fprintf(stderr, " no\n");
   }
   fprintf(stderr, "Full resolution preview %s\n", state->fullResPreview ? "Yes": "No");

   fprintf(stderr, "Capture method : ");
   for (i=0; i<next_frame_description_size; i++)
   {
      if (state->frameNextMethod == next_frame_description[i].nextFrameMethod)
         fprintf(stderr, "%s", next_frame_description[i].description);
   }
   fprintf(stderr, "\n\n");

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
      case CommandQuality: // Quality = 1-100
         if (sscanf(argv[i + 1], "%u", &state->quality) == 1)
         {
            if (state->quality > 100)
            {
               fprintf(stderr, "Setting max quality = 100\n");
               state->quality = 100;
            }
            i++;
         }
         else
            valid = 0;
         break;

      case CommandRaw: // Add raw bayer data in metadata
         state->wantRAW = 1;
         break;

      case CommandLink :
      {
         int len = strlen(argv[i+1]);
         if (len)
         {
            state->linkname = malloc(len + 10);
            vcos_assert(state->linkname);
            if (state->linkname)
               strncpy(state->linkname, argv[i + 1], len+1);
            i++;
         }
         else
            valid = 0;
         break;

      }

      case CommandFrameStart:  // use a staring value != 0
      {
         if (sscanf(argv[i + 1], "%d", &state->frameStart) == 1)
         {
            i++;
         }
         else
            valid = 0;
         break;
      }

      case CommandDateTime: // use datetime
         state->datetime = 1;
         break;

      case CommandTimeStamp: // use timestamp
         state->timestamp = 1;
         break;

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

      case CommandThumbnail : // thumbnail parameters - needs string "x:y:quality"
         if ( strcmp( argv[ i + 1 ], "none" ) == 0 )
         {
            state->thumbnailConfig.enable = 0;
         }
         else
         {
            sscanf(argv[i + 1], "%d:%d:%d",
                   &state->thumbnailConfig.width,
                   &state->thumbnailConfig.height,
                   &state->thumbnailConfig.quality);
         }
         i++;
         break;

      case CommandDemoMode: // Run in demo mode - no capture
      {
         // Demo mode might have a timing parameter
         // so check if a) we have another parameter, b) its not the start of the next option
         if (i + 1 < argc  && argv[i+1][0] != '-')
         {
            if (sscanf(argv[i + 1], "%u", &state->demoInterval) == 1)
            {
               // TODO : What limits do we need for timeout?
               state->demoMode = 1;
               i++;
            }
            else
               valid = 0;
         }
         else
         {
            state->demoMode = 1;
         }

         break;
      }

      case CommandEncoding :
      {
         int len = strlen(argv[i + 1]);
         valid = 0;

         if (len)
         {
            int j;
            for (j=0; j<encoding_xref_size; j++)
            {
               if (strcmp(encoding_xref[j].format, argv[i+1]) == 0)
               {
                  state->encoding = encoding_xref[j].encoding;
                  valid = 1;
                  i++;
                  break;
               }
            }
         }
         break;
      }

      case CommandTimelapse:
         if (sscanf(argv[i + 1], "%u", &state->timelapse) != 1)
            valid = 0;
         else
         {
            if (state->timelapse)
               state->frameNextMethod = FRAME_NEXT_TIMELAPSE;
            else
               state->frameNextMethod = FRAME_NEXT_IMMEDIATELY;

            i++;
         }
         break;

      case CommandFullResPreview:
         state->fullResPreview = 1;
         break;

      case CommandKeypress: // Set keypress between capture mode
         state->frameNextMethod = FRAME_NEXT_KEYPRESS;

         if (state->timeout == -1)
            state->timeout = 0;

         break;

      case CommandSignal:   // Set SIGUSR1 & SIGUSR2 between capture mode
         state->frameNextMethod = FRAME_NEXT_SIGNAL;
         // Reenable the signal
         signal(SIGUSR1, default_signal_handler);
         signal(SIGUSR2, default_signal_handler);

         if (state->timeout == -1)
            state->timeout = 0;

         break;

      case CommandGL:
         state->useGL = 1;
         break;

      case CommandGLCapture:
         state->glCapture = 1;
         break;

      case CommandBurstMode:
         state->burstCaptureMode=1;
         break;

      case CommandRestartInterval:
      {
         if (sscanf(argv[i + 1], "%u", &state->restart_interval) == 1)
         {
            i++;
         }
         else
            valid = 0;
         break;
      }

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
 *
 * @param state Pointer to state control struct. camera_component member set to the created camera_component if successful.
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
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
   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera viewfinder format couldn't be set");
      goto error;
   }

   // Set the same format on the video  port (which we don't use here)
   mmal_format_full_copy(video_port->format, format);
   status = mmal_port_format_commit(video_port);

   if (status  != MMAL_SUCCESS)
   {
      vcos_log_error("camera video format couldn't be set");
      goto error;
   }

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
   // Set our stills format on the stills (for encoder) port
   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = VCOS_ALIGN_UP(state->common_settings.width, 32);
   format->es->video.height = VCOS_ALIGN_UP(state->common_settings.height, 16);
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->common_settings.width;
   format->es->video.crop.height = state->common_settings.height;
   format->es->video.frame_rate.num = STILLS_FRAME_RATE_NUM;
   format->es->video.frame_rate.den = STILLS_FRAME_RATE_DEN;

   status = mmal_port_format_commit(still_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera still format couldn't be set");
      goto error;
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   status = mmal_component_enable(camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera component couldn't be enabled");
      goto error;
   }

   if (state->useGL)
   {
      status = raspitex_configure_preview_port(&state->raspitex_state, preview_port);
      if (status != MMAL_SUCCESS)
      {
         fprintf(stderr, "Failed to configure preview port for GL rendering");
         goto error;
      }
   }

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
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct. encoder_component member set to the created camera_component if successful.
 *
 * @return a MMAL_STATUS, MMAL_SUCCESS if all OK, something else otherwise
 */
static MMAL_STATUS_T create_encoder_component(RASPISTILL_STATE *state)
{
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
   MMAL_STATUS_T status;
   MMAL_POOL_T *pool;

   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &encoder);
   assert(status == MMAL_SUCCESS);
   assert( encoder->input_num && encoder->output_num); //vcos_log_error("JPEG encoder doesn't have input/output ports");
   encoder_input = encoder->input[0];
   encoder_output = encoder->output[0];
   // We want same format on input and output
   mmal_format_copy(encoder_output->format, encoder_input->format);
   // Specify out output format
   encoder_output->format->encoding = state->encoding;

   encoder_output->buffer_size = encoder_output->buffer_size_recommended;
   if (encoder_output->buffer_size < encoder_output->buffer_size_min)
      encoder_output->buffer_size = encoder_output->buffer_size_min;

   encoder_output->buffer_num = encoder_output->buffer_num_recommended;

   if (encoder_output->buffer_num < encoder_output->buffer_num_min)
      encoder_output->buffer_num = encoder_output->buffer_num_min;

   status = mmal_port_format_commit(encoder_output);     // Commit the port changes to the output port
   assert(status == MMAL_SUCCESS);
      // Set the JPEG quality level
   status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_JPEG_Q_FACTOR, state->quality);
   assert(status == MMAL_SUCCESS);
   // Set the JPEG restart interval
   status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_JPEG_RESTART_INTERVAL, state->restart_interval);
   if (state->restart_interval && status != MMAL_SUCCESS) {
      vcos_log_error("Unable to set JPEG restart interval");
      assert(0);
   }

   status = mmal_component_enable(encoder);    //  Enable component
   assert(status == MMAL_SUCCESS);

   /* Create pool of buffer headers for the output port to consume */
   pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);
   assert (pool);
   state->encoder_pool = pool;
   state->encoder_component = encoder;

   return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPISTILL_STATE *state)
{
   // Get rid of any port buffers first
   if (state->encoder_pool)
   {
      mmal_port_pool_destroy(state->encoder_component->output[0], state->encoder_pool);
   }

   if (state->encoder_component)
   {
      mmal_component_destroy(state->encoder_component);
      state->encoder_component = NULL;
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
MMAL_PORT_T *camera_still_port = NULL;
MMAL_PORT_T *preview_input_port = NULL;
MMAL_PORT_T *encoder_input_port = NULL;
MMAL_PORT_T *encoder_output_port = NULL;
PORT_USERDATA callback_data;


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
   assert ((status = create_encoder_component(&rstate)) == MMAL_SUCCESS);

   if (rstate.common_settings.verbose)
     fprintf(stderr, "Starting component connection stage\n");

   camera_preview_port = rstate.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
   camera_video_port   = rstate.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
   camera_still_port   = rstate.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
   encoder_input_port  = rstate.encoder_component->input[0];
   encoder_output_port = rstate.encoder_component->output[0];

   if (rstate.common_settings.verbose)
     fprintf(stderr, "Connecting camera stills port to encoder input port\n");

   // Now connect the camera to the encoder
   status = connect_ports(camera_still_port, encoder_input_port, &rstate.encoder_connection);
   assert (status == MMAL_SUCCESS);
      
   // Set up our userdata - this is passed though to the callback where we need the information. Null until we open our filename
   callback_data.file_handle = NULL;
   callback_data.pstate = &rstate;

   // vcos_status = vcos_semaphore_create(&callback_data.complete_semaphore, "RaspiStill-sem", 0);
   vcos_assert(vcos_semaphore_create(&callback_data.complete_semaphore, "RaspiStill-sem", 0) == VCOS_SUCCESS);

   // If GL preview is requested then start the GL threads
   if (rstate.useGL && (raspitex_start(&rstate.raspitex_state) != 0))
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
  vcos_semaphore_delete(&callback_data.complete_semaphore);

  //   mmal_status_to_int(status);

   if (rstate.common_settings.verbose)
     fprintf(stderr, "Closing down\n");
   
   if (rstate.useGL) {
     raspitex_stop(&rstate.raspitex_state);
     raspitex_destroy(&rstate.raspitex_state);
   }
   
   // Disable all our ports that are not handled by connections
   check_disable_port(camera_video_port);
   check_disable_port(encoder_output_port);
   
   if (rstate.preview_connection)
     mmal_connection_destroy(rstate.preview_connection);
   
   if (rstate.encoder_connection)
     mmal_connection_destroy(rstate.encoder_connection);
   
   /* Disable components */
   if (rstate.encoder_component)
     mmal_component_disable(rstate.encoder_component);
   
   if (rstate.preview_parameters.preview_component)
     mmal_component_disable(rstate.preview_parameters.preview_component);
   
   if (rstate.camera_component)
     mmal_component_disable(rstate.camera_component);
   
   destroy_encoder_component(&rstate);
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

