# SPDX-License-Identifier: LGPL-2.1-or-later
cmake_minimum_required(VERSION 3.24)
project(kmediabridge LANGUAGES C)

include(CTest)

option(KMB_ENABLE_HDR_TO_SDR "Build the optional controlled HDR-to-SDR transcoder" OFF)
option(KMB_ENABLE_AVFOUNDATION_TRANSCODE "Build the optional AVC/AAC AVFoundation compatibility transcoder" OFF)
option(KMB_ENABLE_ANDROID_JNI "Build Android JNI entry points into the bridge library" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
if(KMB_ENABLE_HDR_TO_SDR)
    pkg_check_modules(FFMPEG_HDR REQUIRED IMPORTED_TARGET libswscale)
endif()
if(KMB_ENABLE_AVFOUNDATION_TRANSCODE)
    pkg_check_modules(FFMPEG_AVFOUNDATION REQUIRED IMPORTED_TARGET libavfilter libswscale libswresample)
endif()

add_library(
    kmediabridge
    SHARED
    src/kmedia_bridge.c
    src/kmedia_bridge_avfoundation.c
    src/kmedia_bridge_subtitles.c
    src/kmedia_bridge_hdr_math.c
    src/kmedia_bridge_tonemap.c
)
if(KMB_ENABLE_ANDROID_JNI)
    if(NOT ANDROID)
        message(FATAL_ERROR "KMB_ENABLE_ANDROID_JNI requires the Android toolchain")
    endif()
    target_sources(kmediabridge PRIVATE android/kmedia_bridge_android_jni.c)
    target_link_libraries(kmediabridge PRIVATE android log mediandk dl)
endif()
target_include_directories(kmediabridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kmediabridge PRIVATE PkgConfig::FFMPEG)
if(KMB_ENABLE_HDR_TO_SDR)
    target_compile_definitions(kmediabridge PRIVATE KMB_ENABLE_HDR_TO_SDR=1)
    target_link_libraries(kmediabridge PRIVATE PkgConfig::FFMPEG_HDR)
endif()
if(KMB_ENABLE_AVFOUNDATION_TRANSCODE)
    target_compile_definitions(
        kmediabridge
        PRIVATE
            KMB_ENABLE_AVFOUNDATION_TRANSCODE=1
            KMB_ENABLE_SUBTITLE_BURN_IN=1
            KMB_ENABLE_HDR_TO_SDR=1
    )
    target_link_libraries(kmediabridge PRIVATE PkgConfig::FFMPEG_AVFOUNDATION)
endif()
if(UNIX)
    target_link_libraries(kmediabridge PRIVATE m)
endif()
target_compile_features(kmediabridge PRIVATE c_std_11)

if(MSVC)
    target_compile_options(kmediabridge PRIVATE /W4 /WX)
else()
    target_compile_options(kmediabridge PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
endif()

set_target_properties(
    kmediabridge
    PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
        VERSION 0.2.0
        SOVERSION 1
)

if(APPLE)
    set_target_properties(kmediabridge PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(kmediabridge PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

install(TARGETS kmediabridge LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES include/kmedia_bridge.h DESTINATION include)

if(BUILD_TESTING)
    add_executable(kmedia_bridge_timestamps_test tests/kmedia_bridge_timestamps_test.c)
    target_include_directories(
        kmedia_bridge_timestamps_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(kmedia_bridge_timestamps_test PRIVATE PkgConfig::FFMPEG)
    target_compile_features(kmedia_bridge_timestamps_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(kmedia_bridge_timestamps_test PRIVATE /W4 /WX)
    else()
        target_compile_options(kmedia_bridge_timestamps_test PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME kmedia_bridge_timestamps COMMAND kmedia_bridge_timestamps_test)

    add_executable(
        kmedia_bridge_hdr_math_test
        tests/kmedia_bridge_hdr_math_test.c
        src/kmedia_bridge_hdr_math.c
    )
    target_include_directories(
        kmedia_bridge_hdr_math_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_features(kmedia_bridge_hdr_math_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(kmedia_bridge_hdr_math_test PRIVATE /W4 /WX)
    else()
        target_compile_options(kmedia_bridge_hdr_math_test PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(kmedia_bridge_hdr_math_test PRIVATE m)
    endif()
    add_test(NAME kmedia_bridge_hdr_math COMMAND kmedia_bridge_hdr_math_test)

    add_executable(
        kmedia_bridge_avc_encoder_test
        tests/kmedia_bridge_avc_encoder_test.c
    )
    target_include_directories(
        kmedia_bridge_avc_encoder_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(kmedia_bridge_avc_encoder_test PRIVATE PkgConfig::FFMPEG)
    target_compile_features(kmedia_bridge_avc_encoder_test PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(kmedia_bridge_avc_encoder_test PRIVATE /W4 /WX)
    else()
        target_compile_options(kmedia_bridge_avc_encoder_test PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME kmedia_bridge_avc_encoder COMMAND kmedia_bridge_avc_encoder_test)
endif()
