@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "ROOT=%CD%"
set "CXXFLAGS=-std=c++17 -O3 -s -shared -static -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Wall -Wextra -I src"
set "LDFLAGS=-Wl,--gc-sections -Wl,--nxcompat -Wl,--dynamicbase"

rem ---------------------------------------------------------------------------
rem Locate a 64-bit and a 32-bit MinGW-w64 compiler.
rem ---------------------------------------------------------------------------
set "X64_GXX=%LOCALAPPDATA%\Programs\mingw64\bin\g++.exe"
if not exist "%X64_GXX%" set "X64_GXX=%ProgramFiles%\mingw64\bin\g++.exe"
if not exist "%X64_GXX%" set "X64_GXX=C:\msys64\ucrt64\bin\g++.exe"
if not exist "%X64_GXX%" set "X64_GXX=C:\msys64\mingw64\bin\g++.exe"
if not exist "%X64_GXX%" set "X64_GXX=C:\mingw64\bin\g++.exe"

set "X86_GXX=%LOCALAPPDATA%\Programs\mingw32\bin\g++.exe"
if not exist "%X86_GXX%" set "X86_GXX=%ProgramFiles% (x86)\mingw32\bin\g++.exe"
if not exist "%X86_GXX%" set "X86_GXX=C:\msys64\mingw32\bin\g++.exe"
if not exist "%X86_GXX%" set "X86_GXX=C:\mingw32\bin\g++.exe"

if not exist "%X64_GXX%" (
    for /f "delims=" %%I in ('where g++ 2^>nul') do (
        if not defined PATH_GXX set "PATH_GXX=%%~I"
    )
)
if not exist "%X64_GXX%" if defined PATH_GXX set "X64_GXX=%PATH_GXX%"
if not exist "%X86_GXX%" if defined PATH_GXX set "X86_GXX=%PATH_GXX%"

if not exist "%X64_GXX%" (
    echo ERROR: no 64-bit g++ found. Install MinGW-w64 or add g++ to PATH.
    exit /b 1
)
if not exist "%X86_GXX%" (
    echo ERROR: no 32-bit g++ found. Install MinGW-w64 -m32 support or add it to PATH.
    exit /b 1
)

for /f "delims=" %%A in ('"%X64_GXX%" -dumpmachine 2^>nul') do set "X64_TARGET=%%A"
for /f "delims=" %%A in ('"%X86_GXX%" -dumpmachine 2^>nul') do set "X86_TARGET=%%A"
echo Compiler x64: %X64_GXX% (%X64_TARGET%)
echo Compiler x86: %X86_GXX% (%X86_TARGET%)
echo %X64_TARGET% | findstr /b /c:"x86_64" >nul
if errorlevel 1 (
    echo ERROR: the 64-bit compiler is not an x86_64 MinGW-w64 toolchain.
    exit /b 1
)
echo %X86_TARGET% | findstr /b /c:"i686" >nul
if errorlevel 1 (
    echo ERROR: the 32-bit compiler is not an i686 MinGW-w64 toolchain.
    exit /b 1
)
echo.

if not exist "build\x64" mkdir "build\x64"
if not exist "build\x86" mkdir "build\x86"

set "FAILED="

echo [1/4] build\x64\dxgi.dll
"%X64_GXX%" %CXXFLAGS% %LDFLAGS% src\dxgi\dxgi_proxy.cpp src\common\proxy_common.cpp src\dxgi\dxgi.def -o build\x64\dxgi.dll
if errorlevel 1 set "FAILED=1"

echo [2/4] build\x64\d3d9.dll
"%X64_GXX%" %CXXFLAGS% %LDFLAGS% src\d3d9\d3d9_proxy.cpp src\common\proxy_common.cpp src\d3d9\d3d9.def -o build\x64\d3d9.dll
if errorlevel 1 set "FAILED=1"

echo [3/4] build\x86\dxgi.dll
"%X86_GXX%" %CXXFLAGS% %LDFLAGS% -Wl,--enable-stdcall-fixup src\dxgi\dxgi_proxy.cpp src\common\proxy_common.cpp src\dxgi\dxgi.def -o build\x86\dxgi.dll
if errorlevel 1 set "FAILED=1"

echo [4/4] build\x86\d3d9.dll
"%X86_GXX%" %CXXFLAGS% %LDFLAGS% -Wl,--enable-stdcall-fixup src\d3d9\d3d9_proxy.cpp src\common\proxy_common.cpp src\d3d9\d3d9.def -o build\x86\d3d9.dll
if errorlevel 1 set "FAILED=1"

echo.
if defined FAILED (
    echo BUILD FAILED
    exit /b 1
)

echo BUILD OK
for %%F in (build\x64\dxgi.dll build\x64\d3d9.dll build\x86\dxgi.dll build\x86\d3d9.dll) do (
    for %%S in ("%%F") do echo   %%~fS  %%~zS bytes
)
exit /b 0
