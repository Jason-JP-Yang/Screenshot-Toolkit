@echo off
setlocal
rem Configures and builds with the newest installed Visual Studio C++ toolset.
rem Usage: build.cmd [Release|Debug]
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe was not found. Install Visual Studio Build Tools with the C++ workload.
  exit /b 1
)
set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "%VSROOT%"=="" (
  echo No Visual Studio installation with the C++ toolset was found.
  exit /b 1
)

set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "ROOT=%~dp0"
cmake -S "%ROOT%." -B "%ROOT%build\%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >nul || exit /b 1
cmake --build "%ROOT%build\%CONFIG%" || exit /b 1
echo Built %ROOT%build\%CONFIG%\ScreenshotToolkit.exe
