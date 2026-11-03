@echo off
setlocal
rem Builds Release and publishes it to release\<version>. The version lives in CMakeLists.txt.
set "ROOT=%~dp0"
call "%ROOT%build.cmd" Release || exit /b 1
cmake --build "%ROOT%build\Release" --target release || exit /b 1
echo Published to %ROOT%release
