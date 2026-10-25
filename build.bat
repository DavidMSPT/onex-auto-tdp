@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo  Building OneXPlayer APEX Auto TDP Proxy (version.dll)
echo ========================================================

where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] CMake is not found in PATH.
    echo Please install CMake or run from Developer Command Prompt for Visual Studio.
    pause
    exit /b 1
)

if not exist build (
    mkdir build
)

echo.
echo [*] Generating Visual Studio / MSVC build files (x64)...
cmake -B build -A x64
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

echo.
echo [*] Compiling Release build...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

if not exist dist (
    mkdir dist
)

copy /y build\Release\version.dll dist\version.dll >nul 2>nul
if not exist dist\version.dll (
    copy /y build\version.dll dist\version.dll >nul 2>nul
)

if exist dist\version.dll (
    echo.
    echo ========================================================
    echo  SUCCESS! Output binary generated:
    echo  dist\version.dll
    echo ========================================================
    echo Copy 'dist\version.dll' into your OneXConsole directory:
    echo (e.g. C:\Program Files\OneXConsole\ or C:\Program Files (x86)\OneXConsole\)
) else (
    echo [ERROR] Output version.dll not found in expected locations.
)

pause
