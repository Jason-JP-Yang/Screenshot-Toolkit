cmake_minimum_required(VERSION 3.21)
project(ScreenshotToolkit VERSION 0.3.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Git QUIET)
set(SSTK_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)

set(SSTK_DEFINITIONS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000F)
set(SSTK_OPTIONS /W4 /permissive- /utf-8 /Zc:__cplusplus /Gy /Gw $<$<CONFIG:Release>:/O2 /GL /DNDEBUG>)

# The version name, vX.Y.Z[-dev]-<commit>-beta, and the build's UTC time are written on every build, each file only
# when it changes.
add_custom_target(sstk_version
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DVERSION=${PROJECT_VERSION} -DGIT=${GIT_EXECUTABLE}
          -DOUTPUT=${SSTK_GENERATED}/sstk/version.h -DTIME_OUTPUT=${SSTK_GENERATED}/build_time.cpp
          -P ${CMAKE_SOURCE_DIR}/scripts/version.cmake
  BYPRODUCTS ${SSTK_GENERATED}/sstk/version.h ${SSTK_GENERATED}/build_time.cpp
  VERBATIM
)
set_source_files_properties(${SSTK_GENERATED}/build_time.cpp PROPERTIES GENERATED TRUE)

# The icon is drawn in code and baked into an .ico at build time.
add_executable(sstk_iconbake src/brand/iconbake.cpp src/graphics/image.cpp)
target_include_directories(sstk_iconbake PRIVATE include)
target_compile_definitions(sstk_iconbake PRIVATE ${SSTK_DEFINITIONS})
target_compile_options(sstk_iconbake PRIVATE ${SSTK_OPTIONS})
target_link_options(sstk_iconbake PRIVATE $<$<CONFIG:Release>:/LTCG>)
target_link_libraries(sstk_iconbake PRIVATE user32 gdi32 ole32 windowscodecs)
add_custom_command(
  OUTPUT ${SSTK_GENERATED}/app.ico
  COMMAND sstk_iconbake ${SSTK_GENERATED}/app.ico
  DEPENDS sstk_iconbake
  VERBATIM
)
add_custom_target(sstk_icon DEPENDS ${SSTK_GENERATED}/app.ico)

# Headers live in include/sstk/<module>/ and sources in src/<module>/, one module per folder.
add_executable(ScreenshotToolkit WIN32
  src/app/main.cpp
  src/base/platform.cpp
  src/base/product.cpp
  src/base/settings.cpp
  src/base/workers.cpp
  src/capture/capture.cpp
  src/capture/hook.cpp
  src/capture/screen.cpp
  src/capture/window.cpp
  src/editor/art.cpp
  src/editor/brush.cpp
  src/editor/canvas.cpp
  src/editor/chrome.cpp
  src/editor/document.cpp
  src/editor/editor.cpp
  src/editor/jobs.cpp
  src/editor/toolbar.cpp
  src/graphics/compositor.cpp
  src/graphics/image.cpp
  src/graphics/motion.cpp
  src/overlay/bin.cpp
  src/overlay/capsule.cpp
  src/overlay/card.cpp
  src/overlay/dragdrop.cpp
  src/overlay/overlay.cpp
  src/overlay/sprite.cpp
  src/scroll/guard.cpp
  src/scroll/longimage.cpp
  src/scroll/recorder.cpp
  src/scroll/scrollable.cpp
  src/scroll/stitcher.cpp
  src/scroll/tracker.cpp
  src/service/service.cpp
  src/setup/installer.cpp
  src/setup/window.cpp
  src/storage/store.cpp
  src/update/apply.cpp
  src/update/http.cpp
  src/update/json.cpp
  src/update/state.cpp
  src/update/updater.cpp
  res/app.rc
  ${SSTK_GENERATED}/build_time.cpp
)
add_dependencies(ScreenshotToolkit sstk_version sstk_icon)
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS "${SSTK_GENERATED}/app.ico;${SSTK_GENERATED}/sstk/version.h")

target_include_directories(ScreenshotToolkit PRIVATE include ${SSTK_GENERATED})

target_compile_definitions(ScreenshotToolkit PRIVATE
  ${SSTK_DEFINITIONS}
  SSTK_VERSION_NUMBERS=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0
)

target_compile_options(ScreenshotToolkit PRIVATE ${SSTK_OPTIONS})

target_link_options(ScreenshotToolkit PRIVATE
  /MANIFEST:NO
  /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
  # The SYSTEM service must never map the rendering or codec stacks.
  /DELAYLOAD:d3d11.dll /DELAYLOAD:dcomp.dll /DELAYLOAD:dxgi.dll
  /DELAYLOAD:ole32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:windowscodecs.dll
  /DELAYLOAD:dwmapi.dll /DELAYLOAD:psapi.dll /DELAYLOAD:api-ms-win-shcore-scaling-l1-1-1.dll
  /DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll /DELAYLOAD:api-ms-win-core-winrt-error-l1-1-1.dll
  /DELAYLOAD:wtsapi32.dll /DELAYLOAD:userenv.dll /DELAYLOAD:cabinet.dll
  /DELAYLOAD:oleaut32.dll /DELAYLOAD:winhttp.dll /DELAYLOAD:bcrypt.dll
  $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
)

target_link_libraries(ScreenshotToolkit PRIVATE
  user32 gdi32 advapi32 ole32 oleaut32 shell32 uuid
  d3d11 dxgi dcomp dwmapi psapi windowscodecs wtsapi32 userenv cabinet winhttp bcrypt shcore runtimeobject delayimp
)

# Publishes the built program to release/<version>, which is not versioned by git.
add_custom_target(release
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ScreenshotToolkit>
          ${CMAKE_SOURCE_DIR}/release/${PROJECT_VERSION}/ScreenshotToolkit.exe
  DEPENDS ScreenshotToolkit
  VERBATIM
)
