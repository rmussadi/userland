#!/usr/bin/python

import ctypes as ct
import sys
import numpy as np
from PIL import Image
import cv2

width = 600
height = 400
channels = 1
frame_buffer_size = width * height * channels


def create_pil_image():
    # gradient between 0 and 1 for 256*256
    array = np.linspace(0,1,16*16)

    # reshape to 2d
    mat = np.reshape(array,(16,16))
    print np.uint8(mat * 255)
    # Creates PIL image
    img = Image.fromarray(np.uint8(mat * 255) , 'L')
    img.save("joe.png")

create_pil_image()
