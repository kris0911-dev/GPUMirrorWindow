cmake_minimum_required(VERSION 3.20)
project(GPUMirrorWindow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    enable_language(OBJCXX)
endif()

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
    set(MIRROR_IPC src/ipc_win.cpp)
else()
    set(MIRROR_IPC src/ipc_posix.cpp)
endif()

set(MIRROR_RENDERER_SOURCES
    src/renderer/main.cpp
    src/renderer/scene_ui.cpp
    ${MIRROR_IPC}
)

add_executable(mirror_host
    src/chrome.cpp
    ${MIRROR_IPC}
)
add_executable(mirror_video ${MIRROR_RENDERER_SOURCES})
add_executable(mirror_image ${MIRROR_RENDERER_SOURCES})
add_executable(mirror_controls ${MIRROR_RENDERER_SOURCES})
target_compile_definitions(mirror_video PRIVATE MIRROR_APP_SCENE=0)
target_compile_definitions(mirror_image PRIVATE MIRROR_APP_SCENE=1)
target_compile_definitions(mirror_controls PRIVATE MIRROR_APP_SCENE=2)

set(MIRROR_APPS mirror_host mirror_video mirror_image mirror_controls)

if(WIN32)
    target_sources(mirror_host PRIVATE src/host/main_win.cpp src/host/presenter_d3d11.cpp)
    target_link_libraries(mirror_host PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 ws2_32)
    foreach(app mirror_video mirror_image mirror_controls)
        target_sources(${app} PRIVATE src/renderer/producer_d3d11.cpp)
        target_link_libraries(${app} PRIVATE d3d11 dxgi d2d1 dwrite windowscodecs mfplat mfreadwrite mfuuid ole32 ws2_32)
    endforeach()
elseif(APPLE)
    target_sources(mirror_host PRIVATE src/host/main_mac.mm src/host/presenter_metal.mm)
    target_compile_options(mirror_host PRIVATE -fobjc-arc)
    target_link_libraries(mirror_host PRIVATE
        "-framework Cocoa" "-framework Metal" "-framework QuartzCore"
        "-framework IOSurface" "-framework CoreVideo")
    foreach(app mirror_video mirror_image mirror_controls)
        target_sources(${app} PRIVATE src/renderer/producer_metal.mm)
        target_compile_options(${app} PRIVATE -fobjc-arc)
        target_link_libraries(${app} PRIVATE
            "-framework Cocoa" "-framework CoreText" "-framework ImageIO"
            "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
            "-framework IOSurface")
    endforeach()
else()
    message(FATAL_ERROR "GPUMirrorWindow builds on Windows and macOS.")
endif()

set_target_properties(${MIRROR_APPS} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(mirror_host PRIVATE src src/host)
foreach(app mirror_video mirror_image mirror_controls)
    target_include_directories(${app} PRIVATE src src/renderer)
    add_custom_command(TARGET ${app} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:${app}>/assets")
endforeach()
