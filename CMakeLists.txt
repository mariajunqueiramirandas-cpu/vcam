cmake_minimum_required(VERSION 3.21)
project(vcam VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # Static CRT: the media source loads inside Windows services and the tools
    # must run without the VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /guard:cf /external:anglebrackets /external:W0)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    add_link_options(/guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# --- portable core -------------------------------------------------------------
add_library(vcam_core STATIC
    core/src/convert.cpp
    core/src/draw.cpp
    core/src/format.cpp
    core/src/modes.cpp
    core/src/pacer.cpp
    core/src/ring.cpp
    core/src/simd.cpp
)
target_include_directories(vcam_core PUBLIC core/include)
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # The kernels check for SSE4.1 at runtime before they are used.
    set_source_files_properties(core/src/simd.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
endif()

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE vcam_core)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(core_tests PRIVATE Threads::Threads)
endif()
add_test(NAME core_tests COMMAND core_tests)

# --- Windows components --------------------------------------------------------
if(WIN32)
    add_subdirectory(windows)
endif()
