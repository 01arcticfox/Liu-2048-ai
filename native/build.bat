@echo off
setlocal enabledelayedexpansion

set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSDIR%\VC\Tools\MSVC" set "VSDIR=C:\Program Files\Microsoft Visual Studio\17\2022\Community"
if not exist "%VSDIR%\VC\Tools\MSVC" (
  echo [ERROR] Visual Studio C++ build tools not found.
  exit /b 1
)

set "MSVC="
for /d %%D in ("%VSDIR%\VC\Tools\MSVC\14.*") do set "MSVC=%%D"

set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKINC="
for /d %%D in ("%SDKROOT%\Include\10.*") do set "SDKINC=%%~nxD"
if "%SDKINC%"=="" (
  echo [ERROR] Windows SDK not found.
  exit /b 1
)

set "PATH=%MSVC%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKINC%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDKROOT%\Include\%SDKINC%\ucrt;%SDKROOT%\Include\%SDKINC%\um;%SDKROOT%\Include\%SDKINC%\shared;%SDKROOT%\Include\%SDKINC%\winrt;%SDKROOT%\Include\%SDKINC%\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDKROOT%\Lib\%SDKINC%\ucrt\x64;%SDKROOT%\Lib\%SDKINC%\um\x64"

if not exist "%~dp0build" mkdir "%~dp0build"

set "SRC=%~dp0src"
set "OUT=%~dp0build"
set "WV2=%~dp0third_party\WebView2"
set "CFLAGS=/nologo /std:c++20 /Zc:__cplusplus /EHsc /O2 /MT /DNDEBUG /utf-8 /W3 /permissive- /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"%SRC%" /I"%WV2%\include""

echo [1/4] compiling host...
cl %CFLAGS% /c "%SRC%\main.cpp" /Fo"%OUT%\main.obj"
if errorlevel 1 goto :fail

echo [1b] compiling resources...
rc /nologo /fo "%OUT%\app.res" "%~dp0app.rc"
if errorlevel 1 goto :fail

echo [2/4] linking app...
link /nologo /OUT:"%OUT%\Game2048AI.exe" /SUBSYSTEM:WINDOWS /DELAYLOAD:WebView2Loader.dll "%OUT%\main.obj" "%OUT%\app.res" "%WV2%\x64\WebView2Loader.dll.lib" delayimp.lib dwmapi.lib shlwapi.lib ole32.lib user32.lib gdi32.lib shell32.lib
if errorlevel 1 goto :fail

echo [3/4] staging assets...
if exist "%OUT%\web" rmdir /s /q "%OUT%\web"
xcopy /E /I /Y /Q "%~dp0..\web" "%OUT%\web" >nul
copy /Y "%WV2%\x64\WebView2Loader.dll" "%OUT%\" >nul

echo [4/4] building self-test...
cl %CFLAGS% /c "%~dp0tools\selftest.cpp" /Fo"%OUT%\selftest.obj"
if errorlevel 1 goto :fail
link /nologo /OUT:"%OUT%\ai_selftest.exe" /SUBSYSTEM:CONSOLE "%OUT%\selftest.obj"
if errorlevel 1 goto :fail

echo.
echo [OK] run: "%OUT%\Game2048AI.exe"
exit /b 0

:fail
echo.
echo [FAILED] build error
exit /b 1
