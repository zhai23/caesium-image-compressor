@echo off
chcp 65001 >nul
setlocal
title Caesium Registry Cleanup

rem ============================================
rem  Delete Caesium Image Compressor registry
rem  Key: HKCU\Software\SaeraSoft\Caesium Image Compressor
rem  Run this to simulate a first-launch state.
rem ============================================

set "APP_KEY=HKCU\Software\SaeraSoft\Caesium Image Compressor"

reg query "%APP_KEY%" >nul 2>&1
if errorlevel 1 (
    echo [Info] Registry key not found, nothing to delete: %APP_KEY%
    echo.
    pause
    exit /b 0
)

echo The following registry key will be DELETED:
echo   %APP_KEY%
echo.
echo This resets Caesium Image Compressor to its first-launch state.
echo.
set /p confirm="Continue? [Y/N]: "
if /i not "%confirm%"=="y" (
    echo.
    echo Cancelled.
    pause
    exit /b 0
)

reg delete "%APP_KEY%" /f
if errorlevel 1 (
    echo [Error] Delete failed.
    pause
    exit /b 1
)

echo.
echo [OK] Registry key deleted.
echo   The software will now behave as if running for the first time.
echo.
pause
