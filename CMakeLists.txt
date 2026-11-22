cmake_minimum_required(VERSION 3.21)
project(ScreenshotToolkit VERSION 0.1.2 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Headers live in include/sstk/<module>/ and sources in src/<module>/, one module per folder.
add_executable(ScreenshotToolkit WIN32
  src/app/install.cpp
  src/app/main.cpp
  src/base/platform.cpp
  src/base/workers.cpp
  src/capture/capture.cpp
  src/capture/hook.cpp
  src/capture/screen.cpp
  src/graphics/compositor.cpp
  src/graphics/image.cpp
  src/graphics/motion.cpp
  src/overlay/bin.cpp
  src/overlay/capsule.cpp
  src/overlay/card.cpp
  src/overlay/dragdrop.cpp
  src/overlay/overlay.cpp
  src/overlay/sprite.cpp
  src/scroll/longimage.cpp
  src/scroll/recorder.cpp
  src/scroll/scrollable.cpp
  src/scroll/stitcher.cpp
  src/scroll/tracker.cpp
  src/storage/store.cpp
  res/app.rc
)

target_include_directories(ScreenshotToolkit PRIVATE include)

target_compile_definitions(ScreenshotToolkit PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000F
  SSTK_VERSION_NUMBERS=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0
  SSTK_VERSION_TEXT="${PROJECT_VERSION}"
)

target_compile_options(ScreenshotToolkit PRIVATE
  /W4 /permissive- /utf-8 /Zc:__cplusplus /Gy /Gw
  $<$<CONFIG:Release>:/O2 /GL /DNDEBUG>
)

target_link_options(ScreenshotToolkit PRIVATE
  /MANIFEST:NO
  /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
  # The SYSTEM launcher must never map the rendering or codec stacks.
  /DELAYLOAD:d3d11.dll /DELAYLOAD:dcomp.dll /DELAYLOAD:dxgi.dll
  /DELAYLOAD:ole32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:windowscodecs.dll
  /DELAYLOAD:dwmapi.dll /DELAYLOAD:psapi.dll
  /DELAYLOAD:wtsapi32.dll /DELAYLOAD:userenv.dll /DELAYLOAD:cabinet.dll
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

target_link_libraries(ScreenshotToolkit PRIVATE
  user32 gdi32 advapi32 ole32 shell32 uuid
  d3d11 dxgi dcomp dwmapi psapi windowscodecs wtsapi32 userenv cabinet delayimp
)

# Publishes the built program to release/<version>, which is not versioned by git.
add_custom_target(release
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ScreenshotToolkit>
          ${CMAKE_SOURCE_DIR}/release/${PROJECT_VERSION}/ScreenshotToolkit.exe
  DEPENDS ScreenshotToolkit
  VERBATIM
)
