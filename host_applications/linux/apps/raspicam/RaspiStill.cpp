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
#include "RaspiPreview.h"
#include "RaspiCamControl.h"
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

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

enum
{
   FRAME_NEXT_SINGLE,
   FRAME_NEXT_FOREVER,
};

/** Structure containing all state information for the current run */
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


// Our main data storage vessel..
RASPISTILL_STATE rstate;
   
MMAL_PORT_T *camera_preview_port = NULL;
MMAL_PORT_T *camera_video_port = NULL;

// Copied from RaspiPreview.c
void raspipreview_set_defaults(RASPIPREVIEW_PARAMETERS *state)
{
   state->wantPreview = 1;
   state->wantFullScreenPreview = 1;
   state->opacity = 255;
   state->previewWindow.x = 0;
   state->previewWindow.y = 0;
   state->previewWindow.width = 1024;
   state->previewWindow.height = 768;
   state->preview_component = NULL;
}

// copied from RaspiCommonSettings.c
void raspicommonsettings_set_defaults(RASPICOMMONSETTINGS_PARAMETERS *state)
{
   strncpy(state->camera_name, "(Unknown)", MMAL_PARAMETER_CAMERA_INFO_MAX_STR_LEN);
   // We dont set width and height since these will be specific to the app being built.
   state->width = 0;
   state->height = 0;
   state->filename = NULL;
   state->verbose = 0;
   state->cameraNum = 0;
   state->sensor_mode = 0;
   state->gps = 0;
};

// -
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

// -
static MMAL_STATUS_T create_camera_component(RASPISTILL_STATE *state)
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
   MMAL_STATUS_T status;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
   //assert(status == MMAL_SUCCESS);
   raspicamcontrol_set_stereo_mode(camera->output[0], &state->camera_parameters.stereo_mode);
   raspicamcontrol_set_stereo_mode(camera->output[1], &state->camera_parameters.stereo_mode);
   raspicamcontrol_set_stereo_mode(camera->output[2], &state->camera_parameters.stereo_mode);
   //assert (status == MMAL_SUCCESS);

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
         .max_stills_w = (uint32_t)state->common_settings.width,
         .max_stills_h = (uint32_t)state->common_settings.height,
         .stills_yuv422 = 0,
         .one_shot_stills = 1,
         .max_preview_video_w = (uint32_t)state->preview_parameters.previewWindow.width,
         .max_preview_video_h = (uint32_t)state->preview_parameters.previewWindow.height,
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

   raspitex_configure_preview_port(&state->raspitex_state, preview_port);
   assert(status == MMAL_SUCCESS);
   state->camera_component = camera;

   return status;
}

// Destroy the camera component 
static void destroy_camera_component(RASPISTILL_STATE *state)
{
   if (state->camera_component)
   {
      mmal_component_destroy(state->camera_component);
      state->camera_component = NULL;
   }
}

// @param [in][out] frame The last frame number, adjusted to next frame number on output
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

int rs_teardown()
{
   raspitex_stop(&rstate.raspitex_state);
   raspitex_destroy(&rstate.raspitex_state);
   
   // Disable all our ports that are not handled by connections
   check_disable_port(camera_video_port);   
   
   if (rstate.camera_component)
     mmal_component_disable(rstate.camera_component);
   
   //raspipreview_destroy(&rstate.preview_parameters);
   destroy_camera_component(&rstate);
   
   return EX_OK;
}

// -
static void system_init()
{
   bcm_host_init();
   // Register our application with the logging system
   vcos_log_register("RaspiStill", VCOS_LOG_CATEGORY);
   signal(SIGINT, default_signal_handler);
   // Disable USR1 and USR2 for the moment - may be reenabled if go in to signal capture mode
   signal(SIGUSR1, SIG_IGN);
   signal(SIGUSR2, SIG_IGN);
   set_app_name("raspistill");
}

typedef int (*callback_type)(float, float, void*);

unsigned char buffer[] = {1,2,3,4,5,6,7,8,9,10};
int callmeback(callback_type t)
{
   t(2.0,1.0, buffer);
   return 0;
}

static void set_timeout(int duration)
{
   rstate.timeout = duration;
   if (rstate.timeout == -1) {
      rstate.timeout = 5000;
   }
   rstate.frameNextMethod = FRAME_NEXT_SINGLE;
}

extern int set_rectangle(RASPITEX_STATE *state, int x, int y, int width, int height);

// Specified in screen-coords where (0,0) is upper left corner
int draw_rect(int x, int y, int w, int h)  // must be wrt to current window size
{
    return set_rectangle(&rstate.raspitex_state, x,y,w,h);
}

// -
void begin_loop()
{
  int frame;
   
   while (wait_for_next_frame(&rstate, &frame)) {
   } // end for (frame)
   rs_teardown();
}

int start_video(int x, int y, int w, int h, int duration)
{
    system_init();
    default_status(&rstate);

    // Setup for sensor specific parameters
    get_sensor_defaults(rstate.common_settings.cameraNum, rstate.common_settings.camera_name,
			&rstate.common_settings.width, &rstate.common_settings.height);
    set_timeout(duration);
    raspitex_set_window(&rstate.raspitex_state, x, y, w, h);

    raspitex_init(&rstate.raspitex_state);
    // OK, we have a nice set of parameters. Now set up our components. We have 3 components. Camera, Preview and encoder.
    // Camera and encoder are different in stills/video, but preview is the same so handed off to a separate module
    assert (create_camera_component(&rstate) == MMAL_SUCCESS);
    //camera_preview_port = rstate.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port   = rstate.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    //   camera_still_port   = rstate.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];

    assert(raspitex_start(&rstate.raspitex_state) == 0);

    return 0;
}
