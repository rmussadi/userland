#!/usr/bin/python

import ctypes
import sys


def greater_than(a,b):
    if a > b:
        print a,b, 'greater'
        return 1
    else:
        print b,a, 'lesser'
        return 0


callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_float, ctypes.c_float)
callback_func = callback_type(greater_than)

_stilllib = ctypes.CDLL('libtq84.so')
_stilllib.callmeback.argtype = (callback_type)  # Just so python doesn't have to guess the arg types
#_stilllib.callmeback.argtypes = _stilllib.callmeback.argtype = (callback_type) # Just so python doesn't have to guess the arg types

_stilllib.callmeback(callback_func)
