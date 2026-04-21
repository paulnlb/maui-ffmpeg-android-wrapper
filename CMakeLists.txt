cmake_minimum_required(VERSION 3.10)

project(ffmpegwrapper)

include_directories(${CMAKE_SOURCE_DIR}/include/${ANDROID_ABI})

add_library(ffmpegwrapper SHARED ffmpeg_wrapper.c)

add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libavcodec.so)

add_library(avformat SHARED IMPORTED)
set_target_properties(avformat PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libavformat.so)

add_library(avutil SHARED IMPORTED)
set_target_properties(avutil PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libavutil.so)

add_library(swresample SHARED IMPORTED)
set_target_properties(swresample PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/lib/${ANDROID_ABI}/libswresample.so)

target_link_libraries(
    ffmpegwrapper
    avcodec
    avformat
    avutil
    swresample
    log
)
