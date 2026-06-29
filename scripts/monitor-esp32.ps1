param(
    [string]$Port = "",
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ArduinoCli = Join-Path $ProjectRoot "tools\arduino-cli\arduino-cli.exe"
$ArduinoConfig = Join-Path $ProjectRoot "arduino-cli.yaml"

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
    & (Join-Path $ProjectRoot "scripts\setup-arduino.ps1")
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
    throw "Serial port is required."
}

Write-Host "Opening serial monitor on $Port at $Baud..." -ForegroundColor Cyan
Write-Host "Press Ctrl+C to exit." -ForegroundColor Yellow
Write-Host "=========================================="

try {
    $serial = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
    $serial.Open()
    
    while ($true) {
        if ($serial.IsOpen -and $serial.BytesToRead -gt 0) {
            $data = $serial.ReadExisting()
            [Console]::Write($data)
        }
        Start-Sleep -Milliseconds 50
    }
}
catch {
    Write-Host "`n[ERROR] Could not open or read from $Port. It might be in use by another program." -ForegroundColor Red
}
finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
    }
}
