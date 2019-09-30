#!/usr/bin/python

import ctypes as ct
import sys
import numpy as np
from PIL import Image
import cv2

def greater_than(a,b, data):
    npa = np.frombuffer(ct.cast(data, ct.POINTER(ct.c_uint8 * 10)).contents, dtype=np.uint8, count=10).reshape((5,2))
    if a > b:
        print a,b, 'greater'
        print npa *5
    else:
        print b,a, 'lesser'
        print npa

    npa[1,1] = 22
    print npa
    #npa[10,1] = 44
    return 1

x=1

def joe_gots_a_frame(data):
    global x
    print "frame", x
    x = x+1
    return x
    
_stilllib = ct.CDLL('libtq84.so')


callback_type = ct.CFUNCTYPE(ct.c_int, ct.c_float, ct.c_float, ct.POINTER(ct.c_byte))
callback_func = callback_type(greater_than)
_stilllib.callmeback.argtype = (callback_type)  # Just so python doesn't have to guess the arg types
#_stilllib.callmeback.argtypes = _stilllib.callmeback.argtype = (callback_type) # Just so python doesn't have to guess the arg types
_stilllib.callmeback(callback_func)

# typedef int (*buffer_cb_type)(unsigned char *);
callback_type = ct.CFUNCTYPE(ct.c_int, ct.POINTER(ct.c_byte))
callback_func = callback_type(joe_gots_a_frame)
_stilllib.set_glbuff_cb.argtype = (callback_type)  # Just so python doesn't have to guess the arg types
_stilllib.set_glbuff_cb(callback_func)

_stilllib.start_video(0,0,1024,1024, 10000)
_stilllib.set_glbuff_cb(callback_func)
_stilllib.begin_loop()



def create_pil_image():
    # gradient between 0 and 1 for 256*256
    array = np.linspace(0,1,256*256)

    # reshape to 2d
    mat = np.reshape(array,(256,256))

    # Creates PIL image
    img = Image.fromarray(np.uint8(mat * 255) , 'L')
    img.save("joe.bmp")
