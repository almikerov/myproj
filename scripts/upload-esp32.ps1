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
$BuildPath = Join-Path $ProjectRoot ".arduino\build\$SafeFqbn"

function Show-SerialPorts {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    if ($ports.Count -eq 0) {
        Write-Host "No COM ports detected by Windows." -ForegroundColor Yellow
        return
    }

    Write-Host "Detected COM ports:" -ForegroundColor Cyan
    foreach ($detectedPort in $ports) {
        Write-Host "  $detectedPort"
    }
}

if (!(Test-Path -LiteralPath $ArduinoCli) -or !(Test-Path -LiteralPath $ArduinoConfig)) {
    & (Join-Path $ProjectRoot "scripts\setup-arduino.ps1") -Fqbn $Fqbn
}

if (!$SkipCompile) {
    & (Join-Path $ProjectRoot "scripts\compile-esp32.ps1") -Fqbn $Fqbn
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $availablePorts = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($availablePorts.Count -eq 1) {
        $Port = $availablePorts[0]
        Write-Host "Auto-selected only available COM port: $Port" -ForegroundColor Green
    } else {
        Show-SerialPorts
        $Port = Read-Host "Enter ESP32 port, for example COM3"
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
