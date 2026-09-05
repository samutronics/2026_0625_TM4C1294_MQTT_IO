@echo off
REM Create the CC35x1 flash binary WITHOUT programming the board.
REM
REM Re-signs the flash images from the current build (mqtt_io_cc35x1/Debug) and
REM produces:
REM   - the signed image set in  mqtt_io_cc35x1/Debug/toolbox/  (used by flash.bat)
REM   - one OTA artifact in       mqtt_io_cc35x1/Debug/ota/<fingerprint>.bin
REM
REM Prereq: build the CCS project once so Debug/mqtt_io_cc35x1.out + Debug/syscfg exist.
REM After this, run flash.bat to program the board with the created binary.
REM Uses Git Bash specifically (not WSL) for proper Windows path mapping.

cd /d "%~dp0"
echo [createbin.bat] Creating signed CC35x1 binary via Git Bash (no hardware)...
echo.
"C:\Program Files\Git\bin\bash.exe" "%~dp0../platform/cc35x1/tools/flash.sh" --sign-only %*
set rc=%errorlevel%
echo.
if %rc% equ 0 (
    echo [createbin.bat] Binary created successfully. Run flash.bat to program the board.
) else (
    echo [createbin.bat] Binary creation failed with error code %rc%.
)
REM Only pause for interactive (double-click) use; CCS post-build sets CCS_BUILD
REM so the build never hangs waiting on a keypress.
if not defined CCS_BUILD pause
exit /b %rc%
