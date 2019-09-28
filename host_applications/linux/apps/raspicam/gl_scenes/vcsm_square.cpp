/* Make the render output CPU accessible by defining a framebuffer texture
 * stored in a VCSM (VideoCore shared memory) EGL image.
 *
 * This example just demonstrates how to use use the APIs by using the CPU.
 * to blit an animated rectangle into frame-buffer texture in shared memory.
 *
 * A more realistic example would be to do a blur, edge-detect in GLSL then pass
 * the buffer to OpenCV. There may be some benefit in using multiple GL contexts
 * to reduce the impact of serializing operations with a glFlush.
 *
 * N.B VCSM textures are raster scan order textures. This makes it very
 * convenient to read and modify VCSM frame-buffer textures from the CPU.
 * However, if the output of the CPU stage is drawn again as a texture that
 * is rotated or scaled then it can sometimes be better to use glTexImage2D
 * to allow the driver to convert this back into the native texture format.
 *
 * Example usage
 * raspistill -p 0,0,1024,1024 -gw 0,0,1024,1024 -t 10000 --gl -gs vcsm_square
 */
/* Uncomment the next line to compare with the glReadPixels implementation. VCSM
 * should run at about 40fps with a 1024x1024 texture compared to about 20fps
 * using glReadPixels.
 */
//#define USE_READPIXELS

#include "vcsm_square.h"
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "RaspiTex.h"
#include "RaspiTexUtil.h"
#include "user-vcsm.h"

/* Draw a scaled quad showing the entire texture with the
 * origin defined as an attribute */
static RASPITEXUTIL_SHADER_PROGRAM_T vcsm_square_oes_shader =
{
    .vertex_source =
    "attribute vec2 vertex;\n"
    "varying vec2 texcoord;\n"
    "void main(void) {\n"
    "   texcoord = 0.5 * (vertex + 1.0);\n" \
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n"
    "}\n",

    .fragment_source =
    "#extension GL_OES_EGL_image_external : require\n"
    "uniform samplerExternalOES tex;\n"
    "varying vec2 texcoord;\n"
    "void main(void) {\n"
    "    gl_FragColor = texture2D(tex, texcoord);\n"
    "}\n",
    .uniform_names = {"tex"},
    .attribute_names = {"vertex"},
};
static RASPITEXUTIL_SHADER_PROGRAM_T vcsm_square_shader =
{
    .vertex_source =
    "attribute vec2 vertex;\n"
    "varying vec2 texcoord;\n"
    "void main(void) {\n"
    "   texcoord = 0.5 * (vertex + 1.0);\n" \
    "   gl_Position = vec4(vertex, 0.0, 1.0);\n"
    "}\n",

    .fragment_source =
    "uniform sampler2D tex;\n"
    "varying vec2 texcoord;\n"
    "void main(void) {\n"
    "    gl_FragColor = texture2D(tex, texcoord);\n"
    "}\n",
    .uniform_names = {"tex"},
    .attribute_names = {"vertex"},
};

static RASPITEXUTIL_SHADER_PROGRAM_T line_shader =
{
    .vertex_source =
    "attribute vec2 vertex; \n"
    "varying vec3 color;\n"
   "void main() \n"
   "{ \n"
    "   gl_Position = vec4(vertex, 0.9, 1.0);\n"
    "   color = vec3(1.0, 0.0, 0.0);"
    "   if (vertex.x > 0.0) {"
         "   color = vec3(1.0, 0.0, 1.0);"
        "}"
   "} \n",
    
    .fragment_source =
    "precision mediump float; \n"
    "varying vec3 color;\n"
    "void main() \n"
    "{ \n"
    " gl_FragColor = vec4(color, 1.0); \n"
    "}\n",
    .attribute_names = {"vertex"},
};

    

static GLfloat quad_varray[] = {
   -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, -1.0f,
   -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f,
};

static GLuint quad_vbo;
static GLuint line_vbo;

static struct egl_image_brcm_vcsm_info vcsm_info;
static EGLImageKHR eglFbImage;
static GLuint fb_tex_name;
static GLuint fb_name;


// VCSM buffer dimensions must be a power of two. Use glViewPort to draw NPOT
// rectangles within the VCSM buffer.
static int fb_width = 1024;
static int fb_height = 1024;

static const EGLint vcsm_square_egl_config_attribs[] =
{
    EGL_RED_SIZE,   8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE,  8,
    EGL_ALPHA_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
};

static int square_idx = 0;
GLfloat line_varray[8*10];

int set_rectangle(RASPITEX_STATE *state, int x, int y, int width, int height)
{
  // convert from screen coordinates to NDC
  GLfloat ndcx1 =  2*((GLfloat)x/(GLfloat)state->width) - 1;
  GLfloat ndcy1 = -2*((GLfloat)y/(GLfloat)state->height) +1;

  GLfloat ndcx2 =  2*(((GLfloat)x+(GLfloat)width)/(GLfloat)state->width) - 1;
  GLfloat ndcy2 = -2*((GLfloat)(y+height)/(GLfloat)state->height) +1;

  int idx = square_idx * 8;
    
  line_varray[idx+0] = ndcx1;
  line_varray[idx+1] = ndcy2;

  line_varray[idx+2] = ndcx1;
  line_varray[idx+3] = ndcy1;

  line_varray[idx+4] = ndcx2;
  line_varray[idx+5] = ndcy1;

  line_varray[idx+6] = ndcx2;
  line_varray[idx+7] = ndcy2;
  int lidx = square_idx;
  square_idx++;
  
  //  for(int x=0; x< 8; x+=2) {
  //  printf("%f, %f  ", line_varray[x], line_varray[x+1]);
  //}
  //printf("\n");
  return lidx;
}

static void init_vcsm_rectangle()
{
    GLCHK(glGenBuffers(1, &line_vbo));
}

static void draw_vcsm_rectangle(int idx)
{
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, line_vbo));
    GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat)*8, &line_varray[idx*8], GL_STATIC_DRAW));
    GLCHK(glUseProgram(line_shader.program));
    GLCHK(glEnableVertexAttribArray(line_shader.attribute_locations[0]));
    GLCHK(glVertexAttribPointer(line_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0));
    GLCHK(glDrawArrays(GL_LINE_LOOP, 0, 4));
    GLCHK(glDisableVertexAttribArray(line_shader.attribute_locations[0]));
}

// Write the shared memory texture writing something to each line. This is
// just to show that the buffer really is CPU modifiable.
static int vcsm_square_draw_pattern(unsigned char *buffer)
{
    static unsigned x_offset;

    unsigned char *line_start = (unsigned char *) buffer;
    unsigned width = fb_width > 32 ? 32 : fb_width;
    int i = 0;
    size_t stride = fb_width  << 2;

    x_offset = (x_offset + 1) % (fb_width - width);
    for (i = 0; i < fb_height; i++) {
        memset(line_start + (x_offset << 2), ~0, width << 2);
        line_start += stride;
    }
    return 0;
}

static int do_nothing(unsigned char *buffer)
{
  return 0;
}

typedef int (*buffer_cb_type)(unsigned char *);

buffer_cb_type glbuff_cb = NULL;
int set_glbuff_cb(buffer_cb_type callback_fn)
{
  glbuff_cb = callback_fn;
  return 0;
}

//
void publish_buffer(unsigned char *vcsm_buffer)
{
    if(glbuff_cb == NULL) {
        vcsm_square_draw_pattern(vcsm_buffer);
    } else {
        glbuff_cb(vcsm_buffer);
    }
}


static int vcsm_square_init(RASPITEX_STATE *raspitex_state)
{
    int rc = vcsm_init();
    vcos_log_trace("%s: vcsm_init %d", VCOS_FUNCTION, rc);

    raspitex_state->egl_config_attribs = vcsm_square_egl_config_attribs;
    rc = raspitexutil_gl_init_2_0(raspitex_state);

    if (rc != 0)
        goto end;

    // Shader for drawing the YUV OES texture
    rc = raspitexutil_build_shader_program(&vcsm_square_oes_shader);
    GLCHK(glUseProgram(vcsm_square_oes_shader.program));
    GLCHK(glUniform1i(vcsm_square_oes_shader.uniform_locations[0], 0)); // tex unit

    // Shader for drawing VCSM sampler2D texture
    rc = raspitexutil_build_shader_program(&vcsm_square_shader);
    GLCHK(glUseProgram(vcsm_square_shader.program));
    GLCHK(glUniform1i(vcsm_square_shader.uniform_locations[0], 0)); // tex unit

    // Shader for lines
    rc = raspitexutil_build_shader_program(&line_shader);
    GLCHK(glUseProgram(line_shader.program));
    //    GLCHK(glUniform1i(line_shader.uniform_locations[0], 0)); 
    
    // --------- Frame Buffer stuff -----------
    GLCHK(glGenFramebuffers(1, &fb_name));   // Create a new FBO object
    GLCHK(glBindFramebuffer(GL_FRAMEBUFFER, fb_name));  // Make it active

    GLCHK(glGenTextures(1, &fb_tex_name));   // Create a new OpenGL Texture object
    GLCHK(glBindTexture(GL_TEXTURE_2D, fb_tex_name));  // Make it active

    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));

    printf("Using VCSM\n");
    vcsm_info.width = fb_width;
    vcsm_info.height = fb_height;

    eglFbImage = eglCreateImageKHR(raspitex_state->display, EGL_NO_CONTEXT, EGL_IMAGE_BRCM_VCSM, &vcsm_info, NULL);
    if (eglFbImage == EGL_NO_IMAGE_KHR || vcsm_info.vcsm_handle == 0) {
        vcos_log_error("%s: Failed to create EGL VCSM image\n", VCOS_FUNCTION);  return -1;
    }
    GLCHK(glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, eglFbImage));

    GLCHK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex_name, 0));
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        vcos_log_error("GL_FRAMEBUFFER is not complete\n");	return -1;
    }

    // --------- QUAD VBO stuff -----------
    GLCHK(glGenBuffers(1, &quad_vbo));
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glBufferData(GL_ARRAY_BUFFER, sizeof(quad_varray), quad_varray, GL_STATIC_DRAW));

    // --------- LINE VBO stuff -----------
    init_vcsm_rectangle();

    GLCHK(glClearColor(0.1f, 0.1f, 0.1f, 0.5));

    set_glbuff_cb(do_nothing);
end:
    return rc;
}

static int vcsm_square_redraw(RASPITEX_STATE *raspitex_state)
{
    unsigned char *vcsm_buffer = NULL;
    VCSM_CACHE_TYPE_T cache_type;

    vcos_log_trace("%s", VCOS_FUNCTION);

    //glClearColor(255, 0, 0, 255);
    // Clear screen frame buffer which is currently bound 

    // ------ Fill the viewport with the camera image
    // --------------  Bind our Virtual FBO and clear it out
    GLCHK(glBindFramebuffer(GL_FRAMEBUFFER, fb_name));
    GLCHK(glViewport(0, 0, fb_width, fb_height));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    GLCHK(glUseProgram(vcsm_square_oes_shader.program));
    GLCHK(glActiveTexture(GL_TEXTURE0));
    GLCHK(glBindTexture(GL_TEXTURE_EXTERNAL_OES, raspitex_state->y_texture)); // bind Y-Texture(oppaque ptr from cam)
    GLCHK(glBindBuffer(GL_ARRAY_BUFFER, quad_vbo));
    GLCHK(glEnableVertexAttribArray(vcsm_square_oes_shader.attribute_locations[0]));
    GLCHK(glVertexAttribPointer(vcsm_square_oes_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0));
    GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));
    GLCHK(glFinish());

    // Make the buffer CPU addressable with host cache enabled and Modify it
    vcsm_buffer = (unsigned char *) vcsm_lock_cache(vcsm_info.vcsm_handle, VCSM_CACHE_TYPE_HOST, &cache_type);
    if (! vcsm_buffer) { vcos_log_error("Failed to lock VCSM buffer for handle %d\n", vcsm_info.vcsm_handle); return -1;}
    vcos_log_trace("Locked vcsm handle %d at %p\n", vcsm_info.vcsm_handle, vcsm_buffer);
    //vcsm_square_draw_pattern(vcsm_buffer);
    publish_buffer(vcsm_buffer);
    vcsm_unlock_ptr(vcsm_buffer); // Release the locked texture memory to flush the CPU cache and allow GPU to read it

    // Draw the modified texture buffer to the screen
    GLCHK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    GLCHK(glViewport(raspitex_state->x, raspitex_state->y, raspitex_state->width, raspitex_state->height));
    GLCHK(glUseProgram(vcsm_square_shader.program));
    GLCHK(glBindTexture(GL_TEXTURE_2D, fb_tex_name));
    GLCHK(glEnableVertexAttribArray(vcsm_square_shader.attribute_locations[0]));
    GLCHK(glVertexAttribPointer(vcsm_square_shader.attribute_locations[0], 2, GL_FLOAT, GL_FALSE, 0, 0));
    GLCHK(glDrawArrays(GL_TRIANGLES, 0, 6));
    GLCHK(glDisableVertexAttribArray(vcsm_square_shader.attribute_locations[0]));

    for(int x =0; x < square_idx; x++) {
        draw_vcsm_rectangle(x);
    }

    GLCHK(glUseProgram(0));

    return 0;
}

int vcsm_square_open(RASPITEX_STATE *raspitex_state)
{
    vcos_log_trace("%s", VCOS_FUNCTION);

    raspitex_state->ops.gl_init = vcsm_square_init;
    raspitex_state->ops.redraw = vcsm_square_redraw;
    raspitex_state->ops.update_y_texture = raspitexutil_update_y_texture;
    return 0;
}
