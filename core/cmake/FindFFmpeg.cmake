# 模块13：只找"NDK + c++_shared 友好"的 FFmpeg 预编译包。
# 校验点：NEEDED 里必须是 libc++_shared.so（不是 libstdc++），否则运行期 abort。
set(FFMPEG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/ffmpeg/${ANDROID_ABI}"
    CACHE PATH "FFmpeg 预编译根目录")

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS ${FFMPEG_ROOT}/include)
foreach(lib avformat avcodec avutil swresample)
  find_library(FFMPEG_${lib}_LIBRARY NAMES ${lib} HINTS ${FFMPEG_ROOT}/lib)
endforeach()

if(FFMPEG_INCLUDE_DIR AND FFMPEG_avcodec_LIBRARY)
  add_library(avformat  UNKNOWN IMPORTED)
  set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${FFMPEG_avformat_LIBRARY})
  include_directories(${FFMPEG_INCLUDE_DIR})
  message(STATUS "FFmpeg ok: ${FFMPEG_ROOT}")
else()
  message(FATAL_ERROR "缺 FFmpeg 预编译包：见 third_party/README.md（或设 YIEYU_OFFLINE_ASSETS=ON 走空实现）")
endif()
