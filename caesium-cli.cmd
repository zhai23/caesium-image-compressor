@echo off
rem Caesium Image Compressor - command line control
rem
rem Place this file next to "Caesium Image Compressor.exe" (or add the
rem application folder to PATH) and use it from any terminal:
rem
rem   caesium-cli status
rem   caesium-cli add ./photos
rem   caesium-cli compress ./photos -q 80 -o ./out --wait
rem
rem The executable is a Windows GUI-subsystem program, so in an interactive
rem console its stdout/stderr may not be connected and no output is shown.
rem This wrapper redirects the output through a temporary file (which forces
rem cmd to wire up the standard handles) and then echoes it back, so it works
rem reliably in cmd, PowerShell and Windows Terminal.
rem
rem All output uses the system's local encoding (GBK on Chinese Windows), so
rem both the human-readable table and --json display correctly in a Chinese
rem console without any codepage switching. Scripts that need strict UTF-8 can
rem set CAESIUM_CLI_UTF8=1 before invoking the command.

setlocal
if not exist "%~dp0Caesium Image Compressor.exe" (
    echo Error: could not find "%~dp0Caesium Image Compressor.exe" 1>&2
    exit /b 1
)

set "OUTFILE=%TEMP%\caesium-cli-%RANDOM%-%RANDOM%.out"
"%~dp0Caesium Image Compressor.exe" --cli %* > "%OUTFILE%" 2>&1
set "EXIT_CODE=%ERRORLEVEL%"
if exist "%OUTFILE%" (
    type "%OUTFILE%"
    del "%OUTFILE%" >nul 2>&1
)
exit /b %EXIT_CODE%
