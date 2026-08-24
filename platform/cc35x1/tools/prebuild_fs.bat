@echo off
REM ===========================================================================
REM  Regenerate the CC35x1 embedded web filesystem before every build.
REM
REM  The CC35x1 httpd serves its pages from platform/cc35x1/fsdata.c, which
REM  fs.c pulls in via `#include HTTPD_FSDATA_FILE` (= "fsdata.c").  CCS resolves
REM  that include against the PROJECT-ROOT copy mqtt_io_cc35x1\fsdata.c (the
REM  projectspec copies it in with action="copy"), so BOTH files must be
REM  refreshed from mqtt_io/fs/ or HTML edits never reach the firmware.
REM
REM  NOTE: Do NOT regenerate mqtt_io/io_fsdata.h (the TM4C header) here.
REM  TM4C uses lwip-1.4.1 with includes in httpserver_raw/, which differ from
REM  CC35x1's lwip-2.1.3 with includes in lwip/apps/. TM4C io_fsdata.h is
REM  hand-maintained without platform-specific includes (io_fs.c includes them).
REM ===========================================================================
setlocal
cd /d "%~dp0..\..\..\"

set "PY=C:\Users\tomik\AppData\Local\Microsoft\WindowsApps\python3.exe"

"%PY%" platform\cc35x1\tools\makefsdata.py mqtt_io\fs -o platform\cc35x1\fsdata.c
if errorlevel 1 (
  echo WARNING: FS regeneration failed, continuing with existing fsdata.c
  exit /b 0
)

REM Mirror the canonical copy into the project-root copy that fs.c #includes.
copy /y platform\cc35x1\fsdata.c mqtt_io_cc35x1\fsdata.c >nul
echo FS regenerated: platform\cc35x1\fsdata.c -^> mqtt_io_cc35x1\fsdata.c

REM fsdata.c is #included into fs.c, so it is fs.o that must be rebuilt.
if exist "mqtt_io_cc35x1\Debug\fs.o" del "mqtt_io_cc35x1\Debug\fs.o"

exit /b 0
