cmake_minimum_required(VERSION 3.16)
project(replay)

include(FetchContent)
FetchContent_Declare(
    libvmx_ext
    GIT_REPOSITORY https://github.com/openmediatransport/libvmx.git
    # PINNED (W13): `master` re-fetched whatever upstream had that day into a build that measures
    # 10-bit decode at 1 LSB. a1828cb4 (2026-09-23, "Fixed RGB<>YUV constants") is what every
    # measurement of replay v2 was taken on. Move it deliberately, with the replay battery.
    GIT_TAG        a1828cb438823ebb777dcad20f9f56b633ae8db4
)
FetchContent_MakeAvailable(libvmx_ext)
set(LIBVMX_ROOT "${libvmx_ext_SOURCE_DIR}")

# Include sources to compile libvmx natively
add_library(libvmx STATIC
    ${LIBVMX_ROOT}/src/vmxcodec.cpp
    ${LIBVMX_ROOT}/src/vmxcodec_x86.cpp
    ${LIBVMX_ROOT}/src/vmxcodec_avx2.cpp
)
target_include_directories(libvmx PUBLIC ${LIBVMX_ROOT}/src)
target_include_directories(libvmx PRIVATE ${CMAKE_SOURCE_DIR})
set_target_properties(libvmx PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(libvmx PRIVATE "/w" "/WX-")
    set_source_files_properties(${LIBVMX_ROOT}/src/vmxcodec_avx2.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")
else()
    # libvmx's own Linux build (build/buildlinuxx64.sh) passes -mavx2 -mlzcnt -mbmi to every file.
    # Here only the AVX2 file gets -mavx2: the codec picks its x86 or AVX2 path at run time, so
    # the rest has to stay runnable on a CPU without AVX2, as the MSVC build does.
    target_compile_options(libvmx PRIVATE -w -mlzcnt -mbmi
                           -include ${CMAKE_CURRENT_SOURCE_DIR}/libvmx_gcc_compat.h)
    set_source_files_properties(${LIBVMX_ROOT}/src/vmxcodec_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()

set(SOURCES
    replay_codec.cpp
    replay_codec.h
    replay_consumer.cpp
    replay_consumer.h
    replay_export.cpp
    replay_extended_index.h
    replay_producer.cpp
    replay_producer.h
    replay_segmented_storage.cpp
    replay_segmented_storage.h
    replay_source.cpp
    replay_source.h
    replay_store.cpp
    replay_store.h
    replay.cpp
    replay.h
)

casparcg_add_module_project(replay
    SOURCES ${SOURCES}
    INIT_FUNCTION "replay::init"
    UNINIT_FUNCTION "replay::uninit"
)

target_include_directories(replay PRIVATE
    ${CMAKE_SOURCE_DIR}/../../
    ${LIBVMX_ROOT}/src
    ${FFMPEG_INCLUDE_PATH}
)

target_link_libraries(replay PRIVATE
    core
    common
    libvmx
    # FFmpeg, through the CasparCG ffmpeg target that re-exports its libraries: EXPORT encodes
    # in process now (W12) instead of piping into an `ffmpeg` found on PATH.
    ffmpeg
)

set_target_properties(replay PROPERTIES FOLDER modules)
