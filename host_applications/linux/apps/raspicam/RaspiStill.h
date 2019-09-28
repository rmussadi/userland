#ifndef RASPISTILL_H_
#define RASPISTILL_H_

extern "C" {
extern int start_video(int x, int y, int w, int h, int duration);
extern int draw_rect(int x, int y, int w, int h);  // must be wrt to current window size;
extern void begin_loop();
typedef int (*callback_type)(float, float, void*);
extern int callmeback(callback_type t);
}

#endif
