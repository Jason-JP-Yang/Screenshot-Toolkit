@echo off
setlocal
rem Builds Release and publishes it to release\<version>. The version lives in CMakeLists.txt.
for %%i in ("%~dp0..") do set "ROOT=%%~fi\"
call "%~dp0build.cmd" Release release || exit /b 1
echo Published to %ROOT%release
