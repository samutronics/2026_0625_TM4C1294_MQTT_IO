@echo off
REM Flash the CC35x1 board from Windows cmd.exe
REM This batch file invokes the flash.sh bash script with all arguments passed through.
REM Uses Git Bash specifically (not WSL) for proper Windows path mapping.
REM
REM By default, uses ANY XDS110 connected to the PC. To restrict to a specific probe:
REM   set CC35_PROBE_SN=<serial_number>
REM   flash.bat [other args]

cd /d "%~dp0"
echo [flash.bat] Invoking flash.sh via Git Bash...
echo.
REM Use any XDS110 by default (empty CC35_PROBE_SN accepts the first/only probe)
if not defined CC35_PROBE_SN set CC35_PROBE_SN=
"C:\Program Files\Git\bin\bash.exe" "%~dp0platform/cc35x1/tools/flash.sh" %*
set rc=%errorlevel%
echo.
if %rc% equ 0 (
    echo [flash.bat] Flash completed successfully.
) else (
    echo [flash.bat] Flash failed with error code %rc%.
)
pause
exit /b %rc%
