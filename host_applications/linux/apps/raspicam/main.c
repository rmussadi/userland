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

#include "RaspiStill.h"


/**
 * main
 */
int main(int argc, const char **argv)
{
    start_video(0,0,1024,1024, 10000);
    int rec1 = draw_rect(20, 20, 60, 100);  // must be wrt to current window size
    int rec2 = draw_rect(30, 90, 60, 100);  // must be wrt to current window size
    int rec3 = draw_rect(800, 800, 160, 200);  // must be wrt to current window size
    //printf("%d %d %d\n", rec1, rec2, rec3);
    begin_loop();
    return EX_OK;
}

