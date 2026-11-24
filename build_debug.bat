@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Diagnostic build: writes %TEMP%\vsync_proxy_debug.log.<pid> and logs DLL
rem load order and hooks. Never ship these binaries.
set "X64_GXX=%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe"
if not exist "%X64_GXX%" set "X64_GXX=%ProgramFiles%\mingw64\bin\g++.exe"
if not exist "%X64_GXX%" set "X64_GXX=C:\msys64\ucrt64\bin\g++.exe"

set "X86_GXX=%LOCALAPPDATA%\Programs\mingw32\bin\g++.exe"
if not exist "%X86_GXX%" set "X86_GXX=C:\msys64\mingw32\bin\g++.exe"

set "CXXFLAGS=-std=c++17 -O1 -g -shared -static -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DVSYNC_DEBUG_LOG -Wall -Wextra -I src"
set "LDFLAGS=-Wl,--gc-sections -luser32"

if not exist "build\debug\x64" mkdir "build\debug\x64"
if not exist "build\debug\x86" mkdir "build\debug\x86"

echo [1/4] build\debug\x64\dxgi.dll
"%X64_GXX%" %CXXFLAGS% %LDFLAGS% src\dxgi\dxgi_proxy.cpp src\common\proxy_common.cpp src\common\debug_log.cpp src\dxgi\dxgi.def -o build\debug\x64\dxgi.dll
if errorlevel 1 exit /b 1

echo [2/4] build\debug\x64\d3d9.dll
"%X64_GXX%" %CXXFLAGS% %LDFLAGS% src\d3d9\d3d9_proxy.cpp src\common\proxy_common.cpp src\common\debug_log.cpp src\d3d9\d3d9.def -o build\debug\x64\d3d9.dll
if errorlevel 1 exit /b 1

echo [3/4] build\debug\x86\dxgi.dll
"%X86_GXX%" %CXXFLAGS% %LDFLAGS% -Wl,--enable-stdcall-fixup src\dxgi\dxgi_proxy.cpp src\common\proxy_common.cpp src\common\debug_log.cpp src\dxgi\dxgi.def -o build\debug\x86\dxgi.dll
if errorlevel 1 exit /b 1

echo [4/4] build\debug\x86\d3d9.dll
"%X86_GXX%" %CXXFLAGS% %LDFLAGS% -Wl,--enable-stdcall-fixup src\d3d9\d3d9_proxy.cpp src\common\proxy_common.cpp src\common\debug_log.cpp src\d3d9\d3d9.def -o build\debug\x86\d3d9.dll
if errorlevel 1 exit /b 1

echo DEBUG BUILD OK
exit /b 0
