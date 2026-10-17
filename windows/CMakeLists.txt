# Windows components. Windows 11 (build 22000) or later is required at run time
# for MFCreateVirtualCamera; NTDDI_VERSION must say so or the SDK hides it.
add_compile_definitions(NTDDI_VERSION=0x0A00000B)

# Pieces shared by the media source, the SDK and the tool.
add_library(vcam_win STATIC common/common.cpp)
target_include_directories(vcam_win PUBLIC common)
target_link_libraries(vcam_win PUBLIC vcam_core advapi32 ole32)

# The media source Frame Server loads for each camera.
add_library(vcamsource SHARED
    source/activate.cpp
    source/dllmain.cpp
    source/frame_feed.cpp
    source/media_source.cpp
    source/media_stream.cpp
    source/vcamsource.def
)
target_link_libraries(vcamsource PRIVATE vcam_win mfplat mfsensorgroup avrt)

# Producer SDK: a static library for the vcam tool, and vcam.dll + vcam.h for
# everyone else (C ABI).
add_library(vcam_producer STATIC sdk/producer.cpp)
target_include_directories(vcam_producer PUBLIC sdk/include)
target_compile_definitions(vcam_producer PUBLIC VCAM_STATIC)
target_link_libraries(vcam_producer PUBLIC vcam_win)

add_library(vcam_sdk SHARED sdk/producer.cpp)
set_target_properties(vcam_sdk PROPERTIES OUTPUT_NAME vcam)
target_include_directories(vcam_sdk PUBLIC sdk/include)
target_compile_definitions(vcam_sdk PRIVATE VCAM_BUILD_DLL)
target_link_libraries(vcam_sdk PRIVATE vcam_win)

# End-to-end test: drives vcamsource.dll in-process the way Frame Server does.
add_executable(source_test ${PROJECT_SOURCE_DIR}/tests/windows/source_test.cpp)
target_link_libraries(source_test PRIVATE vcam_producer mfplat mfuuid ole32)
add_dependencies(source_test vcamsource)
add_test(NAME source_test COMMAND source_test $<TARGET_FILE:vcamsource>)
