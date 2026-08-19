@echo off
REM Flash the CC35x1 board from Windows cmd.exe
REM This batch file invokes the flash.sh bash script with all arguments passed through.
REM Uses Git Bash specifically (not WSL) for proper Windows path mapping.

cd /d "%~dp0"
echo [flash.bat] Invoking flash.sh via Git Bash...
echo.
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
