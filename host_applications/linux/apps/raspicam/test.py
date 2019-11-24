#!/usr/bin/python

import ctypes as ct
import sys
import numpy as np
from PIL import Image
import cv2

np.set_printoptions(threshold=sys.maxsize)

width = 1024
height = 1024
channels = 1
frame_buffer_size = width * height * channels

x=1

def joe_gots_a_frame(data):
    npa = np.frombuffer(ct.cast(data, ct.POINTER(ct.c_uint8 * frame_buffer_size)).contents, dtype=np.uint8, count=frame_buffer_size).reshape(height,width)
    global x
    if x % 60 == 0:
        print "frame", x
        img = Image.fromarray(np.uint8(npa) , 'L')
        fn = 'myt' + str(x) + '.png'
        img.save(fn)
        #print npa
        #print '----'
    x = x+1
    return x
    
_stilllib = ct.CDLL('libtq84.so')


# typedef int (*buffer_cb_type)(unsigned char *);
callback_type = ct.CFUNCTYPE(ct.c_int, ct.POINTER(ct.c_byte))
callback_func = callback_type(joe_gots_a_frame)
_stilllib.set_glbuff_cb.argtype = (callback_type)  # Just so python doesn't have to guess the arg types
_stilllib.set_glbuff_cb(callback_func)

_stilllib.start_video(0,0,width,height, 10000)
_stilllib.set_glbuff_cb(callback_func)
_stilllib.begin_loop()



def create_pil_image():
    # gradient between 0 and 1 for 256*256
    array = np.linspace(0,1,256*256)

    # reshape to 2d
    mat = np.reshape(array,(256,256))

    # Creates PIL image
    img = Image.fromarray(np.uint8(mat * 255) , 'L')
    img.save("joe.png")
