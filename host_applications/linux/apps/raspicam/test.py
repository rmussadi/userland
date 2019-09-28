#!/usr/bin/python

import ctypes as ct
import sys
import numpy as np
from PIL import Image

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


callback_type = ct.CFUNCTYPE(ct.c_int, ct.c_float, ct.c_float, ct.POINTER(ct.c_byte))
callback_func = callback_type(greater_than)

_stilllib = ct.CDLL('libtq84.so')
_stilllib.callmeback.argtype = (callback_type)  # Just so python doesn't have to guess the arg types
#_stilllib.callmeback.argtypes = _stilllib.callmeback.argtype = (callback_type) # Just so python doesn't have to guess the arg types
_stilllib.callmeback(callback_func)

def create_pil_image():
    # gradient between 0 and 1 for 256*256
    array = np.linspace(0,1,256*256)

    # reshape to 2d
    mat = np.reshape(array,(256,256))

    # Creates PIL image
    img = Image.fromarray(np.uint8(mat * 255) , 'L')
    img.save("joe.bmp")
