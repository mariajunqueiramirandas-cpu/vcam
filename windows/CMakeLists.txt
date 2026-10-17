# Windows components. Windows 11 (build 22000) or later is required at run time
# for MFCreateVirtualCamera; NTDDI_VERSION must say so or the SDK hides it.
add_compile_definitions(NTDDI_VERSION=0x0A00000B)

# Version resources: vcam_version_rc(<target> <file name> <description> <VFT_APP|VFT_DLL>).
function(vcam_version_rc target file_name description file_type)
    set(VCAM_FILE_NAME ${file_name})
    set(VCAM_FILE_DESCRIPTION ${description})
    set(VCAM_FILE_TYPE ${file_type})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
endfunction()

# Pieces shared by the media source, the SDK and the tool.
add_library(vcam_win STATIC common/common.cpp common/feed.cpp)
target_include_directories(vcam_win PUBLIC common)
target_link_libraries(vcam_win PUBLIC vcam_core advapi32 ole32)

# The media source Frame Server loads for each camera.
add_library(vcamsource SHARED
    source/activate.cpp
    source/dllmain.cpp
    source/media_source.cpp
    source/media_stream.cpp
    source/vcamsource.def
)
target_link_libraries(vcamsource PRIVATE vcam_win mfplat mfsensorgroup avrt)
vcam_version_rc(vcamsource vcamsource.dll "vcam media source (loaded by Windows Frame Server)" VFT_DLL)

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
vcam_version_rc(vcam_sdk vcam.dll "vcam producer SDK" VFT_DLL)

# The tool library: installation and management, the frame sources and the
# producer loop. Shared by the command-line tool and the GUI.
add_library(vcam_tool STATIC
    app/capture.cpp
    app/media.cpp
    app/runner.cpp
    app/sources.cpp
    app/system.cpp
    app/util.cpp
)
target_include_directories(vcam_tool PUBLIC app)
target_compile_definitions(vcam_tool PUBLIC VCAM_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(vcam_tool PUBLIC vcam_producer mf mfplat mfreadwrite mfuuid ole32 oleaut32 windowscodecs
                      d3d11 dxgi dwmapi cfgmgr32 shell32 windowsapp)

# The command-line tool.
add_executable(vcam_app app/main.cpp app/vcam.manifest)
# Same file name as the SDK's vcam.dll, so give the debug database its own name.
set_target_properties(vcam_app PROPERTIES OUTPUT_NAME vcam PDB_NAME vcam_exe)
target_link_libraries(vcam_app PRIVATE vcam_tool)
# The manifest above carries the execution level.
target_link_options(vcam_app PRIVATE /MANIFESTUAC:NO)
vcam_version_rc(vcam_app vcam.exe "vcam virtual camera tool" VFT_APP)

# End-to-end test: drives vcamsource.dll in-process the way Frame Server does.
add_executable(source_test ${PROJECT_SOURCE_DIR}/tests/windows/source_test.cpp)
target_link_libraries(source_test PRIVATE vcam_producer mfplat mfuuid ole32)
add_dependencies(source_test vcamsource)
add_test(NAME source_test COMMAND source_test $<TARGET_FILE:vcamsource>)
