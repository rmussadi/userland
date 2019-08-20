#SET(COMPILE_DEFINITIONS -Werror)
#SET( CMAKE_EXE_LINKER_FLAGS "-Wl,--no-as-needed" )include_directories(${PROJECT_SOURCE_DIR}/host_applications/linux/libs/bcm_host/include)
#CFLAGS="$INCS $LIBS `pkg-config --cflags egl`"
#echo $CFLAGS
#export PKG_CONFIG_PATH="/opt/vc/lib/pkgconfig"

PROJECT_SOURCE_DIR='/home/pi/swdev/userland'

INCS="-I $PROJECT_SOURCE_DIR/host_applications/linux/apps/raspicam -I $PROJECT_SOURCE_DIR/host_applications/linux/libs/bcm_host/include -I $PROJECT_SOURCE_DIR/host_applications/linux/libs/sm -I./userland/interface/khronos/include/ -I $PROJECT_SOURCE_DIR/interface/khronos/include/ -I $PROJECT_SOURCE_DIR/"

LIBS="-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lbrcmGLESv2 -lbrcmEGL -lm -ldl -L/opt/vc/lib -lpthread -L $PROJECT_SOURCE_DIR/build/lib/"

gcc  gl_scenes/yuv.c gl_scenes/square.c RaspiCamControl.c RaspiCLI.c RaspiPreview.c RaspiCommonSettings.c RaspiHelpers.c RaspiStill.c RaspiTex.c RaspiTexUtil.c tga.c $INCS $LIBS
