info("Building with FFMpeg requires CMake v3.8+")
cmake_minimum_required(VERSION 3.8.0)
#project(librealsense2 LANGUAGES CXX C )

find_package(FFmpeg REQUIRED)
include_directories(${FFMPEG_INCLUDE_DIRS})
SET(LIBS ${LIBS} ${FFMPEG_LIBRARIES})



