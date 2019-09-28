#ifndef RASPISTILL_H_
#define RASPISTILL_H_

extern int start_video(int x, int y, int w, int h, int duration);
extern int draw_rect(int x, int y, int w, int h);  // must be wrt to current window size;
extern void begin_loop();  


#endif
