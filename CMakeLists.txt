cmake_minimum_required(VERSION 3.21)
project(ScreenshotToolkit LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ScreenshotToolkit WIN32
  src/main.cpp
  src/capsule.cpp
  src/capture.cpp
  src/card.cpp
  src/compositor.cpp
  src/dragdrop.cpp
  src/hook.cpp
  src/image.cpp
  src/install.cpp
  src/motion.cpp
  src/overlay.cpp
  src/platform.cpp
  src/screen.cpp
  src/store.cpp
  res/app.rc
)

target_compile_definitions(ScreenshotToolkit PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000F
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
  /DELAYLOAD:wtsapi32.dll /DELAYLOAD:userenv.dll
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

target_link_libraries(ScreenshotToolkit PRIVATE
  user32 gdi32 advapi32 ole32 shell32 uuid
  d3d11 dxgi dcomp dwmapi psapi windowscodecs wtsapi32 userenv delayimp
)
