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

# A 32-bit build (cmake -A Win32) makes only the DirectShow filter for 32-bit
# applications, and what its test needs.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(VCAM_DSHOW_NAME vcamdshow32)
else()
    set(VCAM_DSHOW_NAME vcamdshow)
endif()

# Pieces shared by the camera backends, the SDK and the tool.
add_library(vcam_win STATIC common/common.cpp common/feed.cpp)
target_include_directories(vcam_win PUBLIC common)
target_link_libraries(vcam_win PUBLIC vcam_core advapi32 ole32)

# Producer SDK as a static library, for the vcam tool and the tests.
add_library(vcam_producer STATIC sdk/producer.cpp)
target_include_directories(vcam_producer PUBLIC sdk/include)
target_compile_definitions(vcam_producer PUBLIC VCAM_STATIC)
target_link_libraries(vcam_producer PUBLIC vcam_win)

# The DirectShow capture filter: the cameras on Windows 10, one build per
# architecture (32-bit applications load vcamdshow32.dll).
add_library(vcamdshow SHARED
    dshow/dllmain.cpp
    dshow/filter.cpp
    dshow/media_types.cpp
    dshow/pin.cpp
    dshow/vcamdshow.def
)
set_target_properties(vcamdshow PROPERTIES OUTPUT_NAME ${VCAM_DSHOW_NAME})
target_link_libraries(vcamdshow PRIVATE vcam_win strmiids ole32 oleaut32 uuid avrt)
vcam_version_rc(vcamdshow ${VCAM_DSHOW_NAME}.dll "vcam camera (DirectShow capture filter)" VFT_DLL)

# End-to-end test: connects the filter's pin to a pretend application's pin.
add_executable(dshow_test ${PROJECT_SOURCE_DIR}/tests/windows/dshow_test.cpp)
target_link_libraries(dshow_test PRIVATE vcam_producer strmiids ole32 oleaut32 uuid)
add_dependencies(dshow_test vcamdshow)
add_test(NAME dshow_test COMMAND dshow_test $<TARGET_FILE:vcamdshow>)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    return()
endif()

# --- 64-bit only below ------------------------------------------------------------

# The media source Frame Server loads for each camera (Windows 11).
add_library(vcamsource SHARED
    source/activate.cpp
    source/dllmain.cpp
    source/media_source.cpp
    source/media_stream.cpp
    source/vcamsource.def
)
target_link_libraries(vcamsource PRIVATE vcam_win mfplat mfsensorgroup avrt)
vcam_version_rc(vcamsource vcamsource.dll "vcam media source (loaded by Windows Frame Server)" VFT_DLL)

# vcam.dll + vcam.h for everyone else (C ABI).
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
