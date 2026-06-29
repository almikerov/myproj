param(
    [string]$Core = "esp32:esp32",
    [string]$Fqbn = "esp32:esp32:esp32s3:FlashSize=16M"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ToolsDir = Join-Path $ProjectRoot "tools"
$ArduinoDir = Join-Path $ToolsDir "arduino-cli"
$ArduinoCli = Join-Path $ArduinoDir "arduino-cli.exe"
$ArduinoHome = Join-Path $ProjectRoot ".arduino"
$ArduinoConfig = Join-Path $ProjectRoot "arduino-cli.yaml"
$DownloadDir = Join-Path $ArduinoHome "downloads"
$TempDir = Join-Path $ArduinoHome "tmp"

function Read-Config {
    param([string]$Path)

    $result = @{}
    if (!(Test-Path -LiteralPath $Path)) {
        return $result
    }

    Get-Content -LiteralPath $Path | ForEach-Object {
        $line = $_.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith("#") -or $line -notmatch "=") {
            return
        }

        $parts = $line -split "=", 2
        $result[$parts[0].Trim()] = $parts[1].Trim()
    }

    return $result
}

function To-YamlPath {
    param([string]$Path)
    return ($Path -replace "\\", "/")
}

function Ensure-Directory {
    param([string]$Path)
    if (!(Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Value)
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Value, $encoding)
}

Ensure-Directory $ArduinoDir
Ensure-Directory $ArduinoHome
Ensure-Directory $DownloadDir
Ensure-Directory $TempDir

$ConfigIni = Read-Config (Join-Path $ProjectRoot "config.ini")
$ProxyUrl = $ConfigIni["PROXY_URL"]
if ([string]::IsNullOrWhiteSpace($ProxyUrl)) {
    $ProxyUrl = $env:PROXY_URL
}
if (![string]::IsNullOrWhiteSpace($ProxyUrl)) {
    Write-Host "Using proxy: $ProxyUrl" -ForegroundColor Cyan
    $env:HTTP_PROXY = $ProxyUrl
    $env:HTTPS_PROXY = $ProxyUrl
    $env:ALL_PROXY = $ProxyUrl
}

if (!(Test-Path -LiteralPath $ArduinoCli)) {
    $Archive = Join-Path $DownloadDir "arduino-cli_latest_Windows_64bit.zip"
    $ExtractDir = Join-Path $TempDir "arduino-cli"
    $Url = "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip"

    if (!(Test-Path -LiteralPath $Archive) -or (Get-Item -LiteralPath $Archive).Length -lt 1000000) {
        Write-Host "Downloading Arduino CLI..." -ForegroundColor Cyan
        if (![string]::IsNullOrWhiteSpace($ProxyUrl)) {
            Invoke-WebRequest -Uri $Url -OutFile $Archive -Proxy $ProxyUrl
        } else {
            Invoke-WebRequest -Uri $Url -OutFile $Archive
        }
    } else {
        Write-Host "Using existing Arduino CLI archive." -ForegroundColor Cyan
    }

    if (Test-Path -LiteralPath $ExtractDir) {
        Remove-Item -LiteralPath $ExtractDir -Recurse -Force
    }

    Write-Host "Installing Arduino CLI into tools/arduino-cli..." -ForegroundColor Cyan
    Expand-Archive -LiteralPath $Archive -DestinationPath $ExtractDir -Force
    $ExtractedCli = Get-ChildItem -LiteralPath $ExtractDir -Recurse -Filter "arduino-cli.exe" | Select-Object -First 1
    if (!$ExtractedCli) {
        throw "arduino-cli.exe was not found in the downloaded archive."
    }
    Copy-Item -LiteralPath $ExtractedCli.FullName -Destination $ArduinoCli -Force
}

$DataDir = Join-Path $ArduinoHome "data"
$UserDir = Join-Path $ArduinoHome "user"
$BuildCacheDir = Join-Path $ArduinoHome "build-cache"
$PackageIndex = Join-Path $DataDir "package_index.json"
$LibraryIndex = Join-Path $DataDir "library_index.json"

Ensure-Directory $DataDir
Ensure-Directory $UserDir
Ensure-Directory $BuildCacheDir

if (!(Test-Path -LiteralPath $PackageIndex)) {
    Write-Utf8NoBom $PackageIndex '{"packages":[]}'
}

if (!(Test-Path -LiteralPath $LibraryIndex)) {
    Write-Utf8NoBom $LibraryIndex '{"libraries":[]}'
}

$ConfigText = @"
directories:
  data: "$(To-YamlPath $DataDir)"
  downloads: "$(To-YamlPath $DownloadDir)"
  user: "$(To-YamlPath $UserDir)"
board_manager:
  additional_urls:
    - "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
"@

Set-Content -LiteralPath $ArduinoConfig -Value $ConfigText -Encoding utf8

Write-Host "Arduino CLI version:" -ForegroundColor Cyan
& $ArduinoCli version

Write-Host "Updating board indexes..." -ForegroundColor Cyan
& $ArduinoCli --config-file $ArduinoConfig core update-index
if ($LASTEXITCODE -ne 0 -and !(Test-Path -LiteralPath (Join-Path $DataDir "package_esp32_index.json"))) {
    throw "Arduino core index update failed and ESP32 index is missing."
}
if ($LASTEXITCODE -ne 0) {
    Write-Host "General Arduino indexes were not fully updated, but ESP32 index is available. Continuing." -ForegroundColor Yellow
}

Write-Host "Installing ESP32 core: $Core" -ForegroundColor Cyan
& $ArduinoCli --config-file $ArduinoConfig core install $Core
if ($LASTEXITCODE -ne 0) {
    throw "ESP32 core installation failed."
}

Write-Host "Arduino environment is ready." -ForegroundColor Green
Write-Host "Default board FQBN: $Fqbn" -ForegroundColor Green

