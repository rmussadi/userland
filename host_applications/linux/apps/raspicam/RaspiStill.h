#ifndef RASPISTILL_H_
#define RASPISTILL_H_

extern "C" {
  extern int start_video(int x, int y, int w, int h, int duration);
  extern void begin_loop();
  extern int draw_rect(int x, int y, int w, int h);  // wrt current window size  
}

#endif
