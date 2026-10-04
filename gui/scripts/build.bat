@echo off
setlocal
cd /d "%~dp0.."
set BUILD_DIR=build

if "%~1"=="clean" (
    echo [GUI] Cleaning old build files...
    if exist %BUILD_DIR% rd /s /q %BUILD_DIR%
)

if not exist %BUILD_DIR% mkdir %BUILD_DIR%

cd %BUILD_DIR%
echo [GUI] Configuring project...
cmake ..
if %ERRORLEVEL% NEQ 0 (
    echo [GUI] CMake configuration failed!
    pause
    exit /b %ERRORLEVEL%
)

echo [GUI] Building project (Debug)...
cmake --build . --config Debug
if %ERRORLEVEL% NEQ 0 (
    echo [GUI] Build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [GUI] Build successful! 
echo Executable is at: %BUILD_DIR%\Debug\GUI.exe
echo.
pause
