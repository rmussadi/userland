CC=g++

PROJECT_SOURCE_DIR=/home/pi/swdev/userland

INCS=-I $(PROJECT_SOURCE_DIR)/host_applications/linux/apps/raspicam -I $(PROJECT_SOURCE_DIR)/host_applications/linux/libs/bcm_host/include -I $(PROJECT_SOURCE_DIR)/host_applications/linux/libs/sm -I./userland/interface/khronos/include/ -I $(PROJECT_SOURCE_DIR)/interface/khronos/include/ -I $(PROJECT_SOURCE_DIR)/ -I $(PROJECT_SOURCE_DIR)/../glm -I $(PROJECT_SOURCE_DIR)/../../glm
LIBS=-lvcsm -lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lbrcmGLESv2 -lbrcmEGL -lm -ldl -L/opt/vc/lib -lpthread -L $(PROJECT_SOURCE_DIR)/build/lib/

CFLAGS=$(INCS) $(LIBS) -g -fpermissive

#ODIR=obj

#_DEPS = hellomake.h
#DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

#_OBJ = hellomake.o hellofunc.o 
#OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))

cam: libtq84.so main.o
	$(CC)  -o $@ main.cpp -ltq84 $(CFLAGS) $(INCS) $(LIBS) -L.

libtq84.so: gl_scenes/vcsm_square.o RaspiCamControl.o RaspiCLI.o RaspiHelpers.o RaspiStill.o RaspiTex.o RaspiTexUtil.o
	$(CC)  $(CFLAGS) -shared gl_scenes/vcsm_square.o RaspiCamControl.o RaspiCLI.o RaspiHelpers.o RaspiStill.o RaspiTex.o RaspiTexUtil.o  -o libtq84.so

%.o: %.cpp $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

hellomake: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f  *.o *~ core $(INCDIR)/*~ cam libtq84.so
