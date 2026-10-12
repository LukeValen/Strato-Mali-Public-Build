# SPDX-License-Identifier: GPL-3.0-or-later
include(ExternalProject)
set(FFMPEG_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg")
file(MAKE_DIRECTORY "${FFMPEG_PREFIX}/include")
get_filename_component(NDK_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
# CMAKE_SYSTEM_VERSION can be reset by dependency projects; the NDK platform
# level is the Android API selected by Gradle/the Android toolchain.
if(NOT ANDROID_PLATFORM_LEVEL MATCHES "^[0-9]+$" OR ANDROID_PLATFORM_LEVEL LESS 21)
    message(FATAL_ERROR "Invalid Android API for the arm64 FFmpeg build: ${ANDROID_PLATFORM_LEVEL}")
endif()
set(FFMPEG_CC "${NDK_BIN}/aarch64-linux-android${ANDROID_PLATFORM_LEVEL}-clang")
if(NOT EXISTS "${FFMPEG_CC}")
    message(FATAL_ERROR "FFmpeg Android compiler not found: ${FFMPEG_CC}")
endif()
find_program(VIDEO_MAKE make REQUIRED)
ExternalProject_Add(strato_video_ffmpeg
    URL https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n6.1.2.tar.gz
    URL_HASH SHA256=9cbefd67b7e4fffde3846699ec03324b7ef789a81c2e9b1059e93719b5b8d189
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=${FFMPEG_PREFIX}
        --enable-cross-compile --target-os=android --arch=aarch64
        --cc=${FFMPEG_CC}
        --ar=${CMAKE_AR} --ranlib=${CMAKE_RANLIB}
        --enable-pic --enable-static --disable-shared --enable-pthreads
        --disable-programs --disable-doc --disable-debug --disable-network
        --disable-autodetect --disable-everything
        --disable-avdevice --disable-avformat --disable-avfilter
        --disable-swresample --disable-swscale
        --enable-avcodec --enable-avutil
        --enable-decoder=h264,vp8,vp9 --enable-parser=h264,vp8,vp9
    BUILD_COMMAND ${VIDEO_MAKE} -j2
    INSTALL_COMMAND ${VIDEO_MAKE} install
    BUILD_BYPRODUCTS "${FFMPEG_PREFIX}/lib/libavcodec.a" "${FFMPEG_PREFIX}/lib/libavutil.a"
)
add_library(strato_video STATIC
    runtime.cpp common/stream.cpp
    video_core/host1x/ffmpeg.cpp video_core/host1x/vic_processor.cpp
    video_core/host1x/codecs/decoder.cpp video_core/host1x/codecs/h264.cpp
    video_core/host1x/codecs/vp8.cpp video_core/host1x/codecs/vp9.cpp
)
add_dependencies(strato_video strato_video_ffmpeg)
set_target_properties(strato_video PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(strato_video PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${FFMPEG_PREFIX}/include")
target_link_libraries(strato_video PRIVATE "${FFMPEG_PREFIX}/lib/libavcodec.a" "${FFMPEG_PREFIX}/lib/libavutil.a" m z)
