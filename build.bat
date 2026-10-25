@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo  Building OneXPlayer APEX Auto TDP Proxy (version.dll)
echo ========================================================

REM 1. Check if cmake is in standard PATH
where cmake >nul 2>nul
if %errorlevel% equ 0 goto FOUND_CMAKE

REM 2. Look for CMake in standard install paths
if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files\CMake\bin;!PATH!"
    goto FOUND_CMAKE
)
if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files (x86)\CMake\bin;!PATH!"
    goto FOUND_CMAKE
)

REM 3. Look for CMake bundled inside Visual Studio using vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do (
        if exist "%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "PATH=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
            echo [*] Found CMake inside Visual Studio installation: %%i
            goto FOUND_CMAKE
        )
    )
)

REM 4. Check typical VS 2022 paths directly
for %%p in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
) do (
    if exist "%%~p\cmake.exe" (
        set "PATH=%%~p;!PATH!"
        echo [*] Found CMake at: %%~p
        goto FOUND_CMAKE
    )
)

echo.
echo [ERROR] CMake is not found on your system.
echo.
echo You have two options:
echo   1. Install CMake via winget:
echo        winget install -e --id Kitware.CMake
echo      (Then restart this command prompt)
echo.
echo   2. Or download the pre-compiled version.dll directly from GitHub Actions / Releases:
echo      https://github.com/DavidMSPT/onex-auto-tdp/releases
echo.
pause
exit /b 1

:FOUND_CMAKE
echo [*] Using CMake from:
where cmake

if not exist build (
    mkdir build
)

echo.
echo [*] Generating Visual Studio / MSVC build files (x64)...
cmake -B build -A x64
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed.
    echo Ensure Visual Studio C++ workload or Build Tools are installed.
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
    echo Copy 'dist\version.dll' and 'autotdp.ini' into your OneXConsole directory:
    echo (e.g. C:\Program Files\OneXConsole\ or C:\Program Files (x86)\OneXConsole\)
) else (
    echo [ERROR] Output version.dll not found in expected locations.
)

pause
