#!/usr/bin/python

import ctypes as ct
import sys
import numpy as np

def greater_than(a,b, data):
    print(np.frombuffer(ct.cast(data, ct.POINTER(ct.c_uint8 * 10)).contents, dtype=np.uint8, count=10).reshape((10,1)))
    #print np.frombuffer(data, ct.POINTER(ct.c_uint8 * 10).contents,dtype=np.uint8, count=10)
    if a > b:
        print a,b, 'greater'
        return 1
    else:
        print b,a, 'lesser'
        return 0


callback_type = ct.CFUNCTYPE(ct.c_int, ct.c_float, ct.c_float, ct.POINTER(ct.c_byte))
callback_func = callback_type(greater_than)

_stilllib = ct.CDLL('libtq84.so')
_stilllib.callmeback.argtype = (callback_type)  # Just so python doesn't have to guess the arg types
#_stilllib.callmeback.argtypes = _stilllib.callmeback.argtype = (callback_type) # Just so python doesn't have to guess the arg types

_stilllib.callmeback(callback_func)
