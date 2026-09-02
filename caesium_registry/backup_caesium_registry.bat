@echo off
chcp 65001 >nul
setlocal
title Caesium Registry Backup

rem ============================================
rem  Backup Caesium Image Compressor registry
rem  Key: HKCU\Software\SaeraSoft\Caesium Image Compressor
rem  The .reg backup is saved in the "backups" subfolder.
rem ============================================

set "APP_KEY=HKCU\Software\SaeraSoft\Caesium Image Compressor"

reg query "%APP_KEY%" >nul 2>&1
if errorlevel 1 (
    echo [Info] Registry key not found: %APP_KEY%
    echo        The software has no saved configuration yet.
    echo.
    pause
    exit /b 1
)

set "BACKUP_DIR=%~dp0backups"
if not exist "%BACKUP_DIR%" mkdir "%BACKUP_DIR%"

for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"`) do set "TS=%%i"

set "OUT=%BACKUP_DIR%\caesium_registry_backup_%TS%.reg"

reg export "%APP_KEY%" "%OUT%" /y
if errorlevel 1 (
    echo [Error] Export failed.
    pause
    exit /b 1
)

echo.
echo [OK] Registry exported successfully.
echo   Source: %APP_KEY%
echo   Saved to: %OUT%
echo.
pause
