# post_build.ps1 — called by Debug/makefile after the linker step.
# Converts mqtt_io.out → timestamped .bin and a fixed-name latest .bin.
#
# File naming matches the fwver SSI tag shown in the web UI (YYYYMMDDHHMM).
# Note: this timestamp is the system time at post-build execution; the
# __DATE__/__TIME__ baked into enet_io.o is set a few seconds earlier during
# compilation, so both round to the same YYYYMMDDHHMM in practice.

param()

$ErrorActionPreference = "Stop"

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$debugDir   = Join-Path $scriptDir "Debug"
$objcopy    = "C:/ti/ccs2100/ccs/tools/compiler/ti-cgt-armllvm_5.1.1.LTS/bin/tiarmobjcopy.exe"

$ts      = Get-Date -Format "yyyyMMddHHmm"
$binName = "mqtt_io_$ts.bin"
$elfPath = Join-Path $debugDir "mqtt_io.out"
$binPath = Join-Path $debugDir $binName
$latest  = Join-Path $debugDir "mqtt_io.bin"

Write-Host ""
Write-Host "Post-build: generating $binName ..." -ForegroundColor Cyan

& $objcopy -O binary $elfPath $binPath
Copy-Item -Force $binPath $latest

$sizeMB = [math]::Round((Get-Item $binPath).Length / 1KB, 1)
Write-Host "Post-build: $binName  ($sizeMB KB)  ->  mqtt_io.bin" -ForegroundColor Green
Write-Host ""
