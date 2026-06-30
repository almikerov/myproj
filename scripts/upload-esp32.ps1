param(
    [string]$Port = "",
    [string]$Fqbn = "esp32:esp32:esp32s3:FlashSize=16M",
    [switch]$SkipCompile
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ArduinoCli = Join-Path $ProjectRoot "tools\arduino-cli\arduino-cli.exe"
$ArduinoConfig = Join-Path $ProjectRoot "arduino-cli.yaml"
$SafeFqbn = $Fqbn -replace "[:/\\=,]", "_"
$LocalCacheRoot = if (![string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    Join-Path $env:LOCALAPPDATA "esp32-cloud-control\arduino"
} else {
    Join-Path $ProjectRoot ".arduino-out"
}
$BuildPath = Join-Path $LocalCacheRoot "build\$SafeFqbn"

if (!(Test-Path -LiteralPath $ArduinoCli) -or !(Test-Path -LiteralPath $ArduinoConfig)) {
    & (Join-Path $ProjectRoot "scripts\setup-arduino.ps1") -Fqbn $Fqbn
}

if (!$SkipCompile) {
    & (Join-Path $ProjectRoot "scripts\compile-esp32.ps1") -Fqbn $Fqbn
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    Write-Host "Available boards and serial ports:" -ForegroundColor Cyan
    $boardList = & $ArduinoCli --config-file $ArduinoConfig board list
    $boardList | ForEach-Object { Write-Host $_ }

    $detectedPorts = @()
    foreach ($line in $boardList) {
        if ($line -match '\b(COM\d+)\b') {
            $detectedPorts += $matches[1]
        }
    }
    $detectedPorts = @($detectedPorts | Sort-Object -Unique)

    if ($detectedPorts.Count -eq 1) {
        $Port = $detectedPorts[0]
        Write-Host "Auto-selected only available Arduino CLI port: $Port" -ForegroundColor Green
    } else {
        $windowsPorts = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique)
        if ($windowsPorts.Count -eq 1) {
            $Port = $windowsPorts[0]
            Write-Host "Arduino CLI did not identify the board, but Windows has one serial port. Auto-selected: $Port" -ForegroundColor Green
        } else {
            if ($windowsPorts.Count -gt 1) {
                Write-Host "Windows serial ports:" -ForegroundColor Cyan
                $windowsPorts | ForEach-Object { Write-Host "  $_" }
            }
            $Port = Read-Host "Enter ESP32 port, for example COM3"
        }
    }
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "Upload port is required."
}

Write-Host "Uploading firmware to $Port..." -ForegroundColor Cyan
& $ArduinoCli `
    --config-file $ArduinoConfig `
    upload `
    --fqbn $Fqbn `
    --port $Port `
    --input-dir $BuildPath `
    $ProjectRoot

if ($LASTEXITCODE -ne 0) {
    throw "Firmware upload failed."
}

Write-Host "Firmware upload completed." -ForegroundColor Green
