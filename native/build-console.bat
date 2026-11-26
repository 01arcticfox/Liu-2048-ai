@echo off
setlocal
rem Build console 2048 AI -> build\2048ai.exe
rem Optional argument renames the output, so a build for experiments can run
rem alongside a batch that is still using the default exe.
cd /d "%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=2048ai"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
  echo [x] Visual Studio C++ toolchain not found
  exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [x] vcvars64 failed
  exit /b 1
)

rem Some sandboxes block reg.exe, so vcvars cannot detect the Windows SDK.
rem Add the SDK include/lib dirs by scanning the folder instead.
set "SDKROOT=%ProgramFiles(x86)%\Windows Kits\10"
if not exist "%SDKROOT%\Include" set "SDKROOT=%ProgramFiles%\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER set "SDKVER=%%d"
if defined SDKVER set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\winrt"
if defined SDKVER set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"

if not exist build mkdir build
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /W3 /I src /Fo:build\ /Fe:build\%OUT%.exe console\main.cpp
if errorlevel 1 (
  echo [x] build failed
  exit /b 1
)
echo [ok] build\%OUT%.exe

rem Keep the exported copy in sync (skip silently if it is locked or missing)
if "%OUT%"=="2048ai" if exist "new__2048__ai" (
  copy /y "build\2048ai.exe" "new__2048__ai\liu 2048ai.exe" >nul 2>&1
  copy /y "console\main.cpp" "new__2048__ai\liu 2048ai.cpp" >nul 2>&1
  copy /y "src\solver\Solver4x4.h" "new__2048__ai\solver\Solver4x4.h" >nul 2>&1
)
