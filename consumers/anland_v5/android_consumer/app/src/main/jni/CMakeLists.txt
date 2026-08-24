cmake_minimum_required(VERSION 3.18)
project(anland_consumer C CXX)

set(LIBDISPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# ---------- Tracy profiler (https://github.com/wolfpld/tracy) ----------
# Client is fetched at configure time and linked in as a static C++ library;
# zones in C code use the TracyC API (tracy/TracyC.h), which is a no-op unless
# TRACY_ENABLE is defined. TRACY_ON_DEMAND keeps the runtime cost near zero
# until a profiler actually connects (adb forward tcp:8086 tcp:8086).
option(ANLAND_TRACY "Build with Tracy profiling support" ON)
if(ANLAND_TRACY)
    include(FetchContent)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tracy)

    add_library(tracy_client STATIC ${tracy_SOURCE_DIR}/public/TracyClient.cpp)
    target_include_directories(tracy_client PUBLIC ${tracy_SOURCE_DIR}/public)
    target_compile_definitions(tracy_client PUBLIC
        TRACY_ENABLE=1
        TRACY_ON_DEMAND=1
    )
endif()

add_library(anland_consumer SHARED
    native_consumer.c
    native_audio.c
    camera_service.c
    ${LIBDISPLAY_DIR}/libdisplay_consumer/display_consumer.c
    ${LIBDISPLAY_DIR}/common/socket_utils.c
)

target_include_directories(anland_consumer PRIVATE
    ${LIBDISPLAY_DIR}/libdisplay_consumer
    ${LIBDISPLAY_DIR}/common
)

target_compile_options(anland_consumer PRIVATE -O3 -flto)
target_link_options(anland_consumer PRIVATE -flto -fuse-ld=lld)

target_link_libraries(anland_consumer
    android
    log
    dl
    EGL
    GLESv2
    aaudio
    camera2ndk
    mediandk
)

if(ANLAND_TRACY)
    target_link_libraries(anland_consumer tracy_client)
endif()

# Root helper for "connect with root" mode. Built as an executable but named
# lib*.so so the Android packager extracts it into the app's nativeLibraryDir
# with execute permission (requires useLegacyPackaging=true). Launched by the
# app via `su -c` to connect the daemon socket and pass the fd back.
add_executable(fdhelper
    fd_helper.c
    ${LIBDISPLAY_DIR}/common/socket_utils.c
)

target_include_directories(fdhelper PRIVATE
    ${LIBDISPLAY_DIR}/common
)

set_target_properties(fdhelper PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "fdhelper"
    SUFFIX ".so"
)

target_link_libraries(fdhelper log)

# Root helper for "immersive mode": grabs the input devices (EVIOCGRAB) and
# streams their events to the app over a bridge socket. Same lib*.so packaging
# trick as fdhelper above -- the app launches it via `su -c` because
# untrusted_app cannot read /dev/input itself.
add_executable(inputgrab
    input_grab.c
    ${LIBDISPLAY_DIR}/common/socket_utils.c
)

target_include_directories(inputgrab PRIVATE
    ${LIBDISPLAY_DIR}/common
)

set_target_properties(inputgrab PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "inputgrab"
    SUFFIX ".so"
)

target_link_libraries(inputgrab log)
