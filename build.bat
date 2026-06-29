@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM ============================================================
REM  Caesium Image Compressor - Portable Build Script
REM  Run this after editing the source to rebuild the portable package.
REM ============================================================

REM --- Move to the directory where this script lives (project root) ---
cd /d "%~dp0"

REM --- Toolchain paths (edit here if your install locations differ) ---
set "QT_DIR=D:\Qt\6.8.0\mingw_64"
set "MINGW_DIR=D:\Qt\Tools\mingw1310_64\bin"

REM --- Workaround env vars discovered during first successful build ---
REM  CMAKE_TLS_VERIFY=0  : CMake's bundled curl has no CA bundle (WinSparkle download)
REM  __COMPAT_LAYER      : stops Windows auto-elevating lupdate.exe/windeployqt.exe (error 740)
set "CMAKE_TLS_VERIFY=0"
set "__COMPAT_LAYER=RunAsInvoker"

REM --- Put the Qt6.8.0-matched MinGW 13.1.0 FIRST on PATH ---
set "PATH=%MINGW_DIR%;%QT_DIR%\bin;%PATH%"

set "BUILD_DIR=build_dir"
set "OUT_DIR=portable\Caesium Image Compressor"

REM --- Optional: pass "clean" to wipe the build dir for a full rebuild ---
if /I "%~1"=="clean" (
    echo [*] Clean requested - removing %BUILD_DIR% ...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

echo.
echo ============================================================
echo  Caesium Image Compressor - Portable Build
echo ============================================================
echo  Qt   : %QT_DIR%
echo  MinGW: %MINGW_DIR%
echo ============================================================
echo.

REM --- Sanity checks ---
if not exist "%QT_DIR%\bin\windeployqt.exe" (
    echo [ERROR] Qt not found at "%QT_DIR%". Edit QT_DIR in this script.
    goto :fail
)
if not exist "%MINGW_DIR%\g++.exe" (
    echo [ERROR] MinGW not found at "%MINGW_DIR%". Edit MINGW_DIR in this script.
    goto :fail
)
where cargo >nul 2>&1 || ( echo [ERROR] cargo not on PATH. Install Rust. & goto :fail )
where cmake >nul 2>&1 || ( echo [ERROR] cmake not on PATH. & goto :fail )

REM ============================================================
REM  Step 1: Configure (only if not already configured)
REM ============================================================
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [1/4] Configuring CMake ...
    cmake -B "%BUILD_DIR%" ^
        -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
        -DPORTABLE=ON ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_C_COMPILER="%MINGW_DIR%\gcc.exe" ^
        -DCMAKE_CXX_COMPILER="%MINGW_DIR%\g++.exe" ^
        -DCMAKE_MAKE_PROGRAM="%MINGW_DIR%\mingw32-make.exe" ^
        -G "MinGW Makefiles"
    if errorlevel 1 goto :fail
) else (
    echo [1/4] CMake already configured ^(skipping^). Use "build.bat clean" to reconfigure.
)

REM ============================================================
REM  Step 2: Build
REM ============================================================
echo.
echo [2/4] Building caesium_image_compressor ...
cmake --build "%BUILD_DIR%" --config Release --target caesium_image_compressor -j
if errorlevel 1 goto :fail

if not exist "%BUILD_DIR%\Caesium Image Compressor.exe" (
    echo [ERROR] Build reported success but exe not found.
    goto :fail
)

REM ============================================================
REM  Step 3: Assemble portable folder + copy native DLLs
REM ============================================================
echo.
echo [3/4] Assembling portable package ...
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

copy /Y "%BUILD_DIR%\Caesium Image Compressor.exe" "%OUT_DIR%\" >nul
if errorlevel 1 goto :fail

set "CAESIUM_DLL=%BUILD_DIR%\libcaesium-prefix\src\libcaesium\target\x86_64-pc-windows-gnu\release\caesium.dll"
if exist "%CAESIUM_DLL%" (
    copy /Y "%CAESIUM_DLL%" "%OUT_DIR%\" >nul
) else (
    echo [ERROR] caesium.dll not found at "%CAESIUM_DLL%".
    goto :fail
)

set "WINSPARKLE_DLL=%BUILD_DIR%\libwinsparkle-prefix\src\libwinsparkle\x64\Release\WinSparkle.dll"
if exist "%WINSPARKLE_DLL%" (
    copy /Y "%WINSPARKLE_DLL%" "%OUT_DIR%\" >nul
) else (
    echo [WARN] WinSparkle.dll not found ^(updater^) - continuing.
)

REM ============================================================
REM  Step 4: Deploy Qt dependencies with windeployqt
REM ============================================================
echo.
echo [4/4] Running windeployqt ...
"%QT_DIR%\bin\windeployqt.exe" --release --compiler-runtime --no-translations ^
    --dir "%OUT_DIR%" "%OUT_DIR%\Caesium Image Compressor.exe"
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  BUILD SUCCEEDED
echo  Portable app: %CD%\%OUT_DIR%\Caesium Image Compressor.exe
echo ============================================================
echo.

REM --- Optionally launch it right away ---
choice /C YN /N /M "Launch the app now? [Y/N] "
if errorlevel 2 goto :done
start "" "%OUT_DIR%\Caesium Image Compressor.exe"

:done
endlocal
exit /b 0

:fail
echo.
echo ============================================================
echo  BUILD FAILED  (see messages above)
echo ============================================================
endlocal
exit /b 1
