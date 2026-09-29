include(ExternalProject)
find_program(WMA_MAKE make REQUIRED)
set(wma_source "${CMAKE_CURRENT_LIST_DIR}/../vendor/ffmpeg")
set(wma_install "${CMAKE_CURRENT_BINARY_DIR}/wma/install")
if(NOT EXISTS "${wma_source}/configure")
    message(FATAL_ERROR "Run horizon-wine/tools/bootstrap-wma.sh before configuring the runtime")
endif()
set(wma_jobs "$ENV{WINE_NX_JOBS}")
if(NOT wma_jobs)
    set(wma_jobs 8)
endif()
ExternalProject_Add(wine-wma
    SOURCE_DIR "${wma_source}"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/wma/build"
    INSTALL_DIR "${wma_install}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND sh <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR> --arch=aarch64 --target-os=none
        --enable-cross-compile --cross-prefix=${DEVKITA64}/bin/aarch64-none-elf-
        --disable-everything --disable-autodetect --disable-programs --disable-doc
        --disable-network --disable-avdevice --disable-avfilter --disable-avformat
        --disable-swscale --disable-postproc --enable-pthreads --disable-w32threads
        --disable-os2threads --disable-shared --enable-static --enable-small
        --enable-avcodec --enable-avutil --enable-swresample
        --enable-decoder=wmav1,wmav2,wmapro,wmalossless
        "--extra-cflags=-O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffixed-x18 -ffunction-sections -fdata-sections -D__SWITCH__ -I${LIBNX}/include"
        "--extra-ldflags=-march=armv8-a+crc+crypto -mtp=soft -fPIE -specs=${LIBNX}/switch.specs -L${LIBNX}/lib"
        "--extra-libs=-lnx -lm"
    BUILD_COMMAND ${WMA_MAKE} -j${wma_jobs}
    INSTALL_COMMAND ${WMA_MAKE} install
    BUILD_BYPRODUCTS "${wma_install}/lib/libavcodec.a"
                     "${wma_install}/lib/libswresample.a"
                     "${wma_install}/lib/libavutil.a")
add_dependencies(wine-nx-runtime wine-wma)
target_include_directories(wine-nx-runtime PRIVATE "${wma_install}/include")
target_link_libraries(wine-nx-runtime PRIVATE "${wma_install}/lib/libavcodec.a"
    "${wma_install}/lib/libswresample.a" "${wma_install}/lib/libavutil.a")
configure_file("${wma_source}/COPYING.LGPLv2.1"
    "${CMAKE_CURRENT_BINARY_DIR}/licenses/FFmpeg-LGPL-2.1.txt" COPYONLY)
