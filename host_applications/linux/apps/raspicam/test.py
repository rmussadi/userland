
import ctypes
import sys

callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_float, ctypes.c_float)

def greater_than(a,b):
    if a > b:
        print a,b, 'greater'
        return 1
    else:
        print b,a, 'lesser'
        return 0


callback_func = callback_type(greater_than)

_callmeback = ctypes.CDLL('libtq84.so')
_callmeback.callmeback.argtype = (callback_type)
#_callmeback.callmeback.argtypes = _callmeback.callmeback.argtype = (callback_type)

_callmeback.callmeback(callback_func)
