param(
    [string]$Fqbn = "esp32:esp32:esp32s3:FlashSize=16M",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ArduinoCli = Join-Path $ProjectRoot "tools\arduino-cli\arduino-cli.exe"
$ArduinoConfig = Join-Path $ProjectRoot "arduino-cli.yaml"
$BuildRoot = Join-Path $ProjectRoot ".arduino\build"
$BuildCache = Join-Path $ProjectRoot ".arduino\build-cache"
$CtagsPath = Join-Path $ProjectRoot "tools\ctags"
$SafeFqbn = $Fqbn -replace "[:/\\=,]", "_"
$BuildPath = Join-Path $BuildRoot $SafeFqbn

if (!(Test-Path -LiteralPath $ArduinoCli) -or !(Test-Path -LiteralPath $ArduinoConfig)) {
    Write-Host "Arduino CLI is not ready. Running setup first..." -ForegroundColor Yellow
    & (Join-Path $ProjectRoot "scripts\setup-arduino.ps1") -Fqbn $Fqbn
}

if ($Clean -and (Test-Path -LiteralPath $BuildPath)) {
    Write-Host "Cleaning firmware build directory..." -ForegroundColor Cyan
    Remove-Item -LiteralPath $BuildPath -Recurse -Force
}

if (!(Test-Path -LiteralPath $BuildPath)) {
    New-Item -ItemType Directory -Path $BuildPath | Out-Null
}

if (!(Test-Path -LiteralPath $BuildCache)) {
    New-Item -ItemType Directory -Path $BuildCache | Out-Null
}

Write-Host "Compiling ESP32 firmware..." -ForegroundColor Cyan
Write-Host "Board: $Fqbn" -ForegroundColor DarkCyan

& $ArduinoCli `
    --config-file $ArduinoConfig `
    compile `
    --fqbn $Fqbn `
    --build-path $BuildPath `
    --build-property "runtime.tools.ctags.path=$CtagsPath" `
    --jobs 0 `
    --warnings default `
    --export-binaries `
    $ProjectRoot

if ($LASTEXITCODE -ne 0) {
    throw "Firmware compilation failed."
}

Write-Host "Firmware compilation completed." -ForegroundColor Green
Write-Host "Build output: $BuildPath" -ForegroundColor Green
