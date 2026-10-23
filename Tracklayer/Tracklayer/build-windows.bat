@echo off
rem Builds Tracklayer.exe. Needs Visual Studio 2022 (Community is free) with the
rem "Desktop development with C++" workload, which includes CMake and Git.
rem Run this from a "Developer Command Prompt for VS 2022" or a normal prompt.

setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake wasn't found. Install Visual Studio 2022 with "Desktop development with C++",
    echo then run this from the "Developer Command Prompt for VS 2022".
    pause
    exit /b 1
)

rem To enable ASIO, add:  -DASIO_SDK_DIR=C:\path\to\asiosdk
cmake -B build -A x64 %*
if errorlevel 1 goto :fail

cmake --build build --config Release --parallel
if errorlevel 1 goto :fail

copy /y "build\Tracklayer_artefacts\Release\Tracklayer.exe" "Tracklayer.exe" >nul
echo.
echo Done: %cd%\Tracklayer.exe
pause
exit /b 0

:fail
echo.
echo Build failed - scroll up for the first error.
pause
exit /b 1
