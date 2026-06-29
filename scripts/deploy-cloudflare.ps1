param(
  [string]$DatabaseName = "esp32-cloud"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $ProjectRoot

$env:XDG_CONFIG_HOME = Join-Path $ProjectRoot ".wrangler-config"
$env:WRANGLER_LOG_PATH = Join-Path $ProjectRoot ".wrangler-logs"
$env:npm_config_cache = Join-Path $ProjectRoot ".npm-cache"

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

$ConfigPath = Join-Path $ProjectRoot "config.ini"
$ConfigIni = Read-Config $ConfigPath

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

$cfToken = $ConfigIni["CLOUDFLARE_API_TOKEN"]
if (![string]::IsNullOrWhiteSpace($cfToken)) {
    $env:CLOUDFLARE_API_TOKEN = $cfToken
}

function Run-Step {
  param(
    [string]$Title,
    [scriptblock]$Command
  )

  Write-Host ""
  Write-Host "== $Title ==" -ForegroundColor Cyan
  & $Command
}

function Get-Wrangler {
  $Wrangler = Join-Path $ProjectRoot "node_modules\.bin\wrangler.cmd"
  if (!(Test-Path $Wrangler)) {
    Run-Step "Installing Wrangler" {
      npm.cmd install --cache $env:npm_config_cache
    }
  }
  return $Wrangler
}

function Update-DatabaseId {
  param([string]$DatabaseId)

  $ConfigPath = Join-Path $ProjectRoot "wrangler.toml"
  $Config = Get-Content -Raw -LiteralPath $ConfigPath
  $Config = $Config -replace 'database_id = "replace-after-running-wrangler-d1-create"', "database_id = `"$DatabaseId`""
  Set-Content -LiteralPath $ConfigPath -Value $Config -Encoding utf8
}

function Read-DatabaseIdFromCreateOutput {
  param([string[]]$Output)

  $Joined = $Output -join "`n"
  $Match = [regex]::Match($Joined, 'database_id\s*=\s*"([^"]+)"')
  if ($Match.Success) {
    return $Match.Groups[1].Value
  }

  $UuidMatch = [regex]::Match($Joined, '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
  if ($UuidMatch.Success) {
    return $UuidMatch.Value
  }

  throw "Could not read D1 database_id from Wrangler output."
}

$Wrangler = Get-Wrangler

Run-Step "Checking Worker syntax" {
  node --check .\cloudflare\worker.js
}

Run-Step "Checking Cloudflare login" {
  $WhoamiOutput = cmd.exe /d /c "`"$Wrangler`" whoami 2>&1"
  $WhoamiText = $WhoamiOutput -join "`n"
  $WhoamiOutput | ForEach-Object { Write-Host $_ }

  if ($LASTEXITCODE -ne 0 -or $WhoamiText -match "not authenticated") {
    if ([string]::IsNullOrWhiteSpace($env:CLOUDFLARE_API_TOKEN)) {
      throw "Cloudflare is not authenticated. Set CLOUDFLARE_API_TOKEN in this PowerShell window and run this script again."
    }

    Write-Host ""
    Write-Host "Using CLOUDFLARE_API_TOKEN from the current PowerShell session." -ForegroundColor Green
  }
}

$ConfigText = Get-Content -Raw -LiteralPath .\wrangler.toml
if ($ConfigText -match 'replace-after-running-wrangler-d1-create') {
  Run-Step "Creating D1 database" {
    $CreateOutput = cmd.exe /d /c "`"$Wrangler`" d1 create `"$DatabaseName`" 2>&1"
    $CreateExitCode = $LASTEXITCODE
    $CreateOutput | ForEach-Object { Write-Host $_ }
    if ($CreateExitCode -ne 0) {
      throw "D1 database creation failed. Check that the API token has Account > D1 > Edit permission for this Cloudflare account."
    }
    $DatabaseId = Read-DatabaseIdFromCreateOutput $CreateOutput
    Update-DatabaseId $DatabaseId
    Write-Host "Saved D1 database_id: $DatabaseId" -ForegroundColor Green
  }
} else {
  Write-Host ""
  Write-Host "D1 database_id is already configured in wrangler.toml." -ForegroundColor Green
}

Run-Step "Applying D1 schema" {
  $D1Output = cmd.exe /d /c "`"$Wrangler`" d1 execute `"$DatabaseName`" --file .\cloudflare\schema.sql --remote 2>&1"
  $D1ExitCode = $LASTEXITCODE
  $D1Output | ForEach-Object { Write-Host $_ }
  if ($D1ExitCode -ne 0) {
    throw "D1 schema apply failed. Check D1 permissions and that the database exists."
  }
}

Run-Step "Applying D1 Migrations (Ignore errors if columns already exist)" {
  $D1MigrateOutput = cmd.exe /d /c "`"$Wrangler`" d1 execute `"$DatabaseName`" --command `"ALTER TABLE devices ADD COLUMN wifi_ssid TEXT DEFAULT ''; ALTER TABLE devices ADD COLUMN wifi_pass TEXT DEFAULT ''; ALTER TABLE devices ADD COLUMN supported_commands TEXT DEFAULT '[]';`" --remote 2>&1"
  $D1MigrateOutput | ForEach-Object { Write-Host $_ }
}

function Update-KvId {
  param([string]$KvId)
  $ConfigPath = Join-Path $ProjectRoot "wrangler.toml"
  $Config = Get-Content -Raw -LiteralPath $ConfigPath
  $Config = $Config -replace 'id = "replace-kv-id"', "id = `"$KvId`""
  Set-Content -LiteralPath $ConfigPath -Value $Config -Encoding utf8
}

$ConfigText = Get-Content -Raw -LiteralPath .\wrangler.toml
if ($ConfigText -match 'replace-kv-id') {
  Run-Step "Creating KV Namespace for Firmware Storage" {
    $KvOutput = cmd.exe /d /c "`"$Wrangler`" kv namespace create FIRMWARE_KV 2>&1"
    $KvOutput | ForEach-Object { Write-Host $_ }
    $Match = [regex]::Match(($KvOutput -join "`n"), 'id\s*=\s*"([^"]+)"')
    if ($Match.Success) {
      Update-KvId $Match.Groups[1].Value
      Write-Host "Saved KV id: $($Match.Groups[1].Value)" -ForegroundColor Green
    } else {
      throw "Failed to create KV namespace or read ID."
    }
  }
}

Run-Step "Configuring secrets" {
  $adminToken = $ConfigIni["ADMIN_TOKEN"]
  $deviceToken = $ConfigIni["DEVICE_BOOTSTRAP_TOKEN"]

  if ([string]::IsNullOrWhiteSpace($adminToken)) {
      throw "ADMIN_TOKEN is missing in config.ini"
  }
  if ([string]::IsNullOrWhiteSpace($deviceToken)) {
      throw "DEVICE_BOOTSTRAP_TOKEN is missing in config.ini"
  }

  Write-Host "Setting ADMIN_TOKEN..." -ForegroundColor Cyan
  $adminToken | & $Wrangler secret put ADMIN_TOKEN
  
  Write-Host "Setting DEVICE_BOOTSTRAP_TOKEN..." -ForegroundColor Cyan
  $deviceToken | & $Wrangler secret put DEVICE_BOOTSTRAP_TOKEN
}

Run-Step "Deploying Worker" {
  $DeployOutput = cmd.exe /d /c "`"$Wrangler`" deploy 2>&1"
  $DeployExitCode = $LASTEXITCODE
  $DeployText = $DeployOutput -join "`n"
  $DeployOutput | ForEach-Object { Write-Host $_ }
  if ($DeployExitCode -ne 0 -and $DeployText -match "fetch failed|connectivity|network|firewall|VPN") {
    throw "Worker deploy failed because Wrangler could not reach Cloudflare reliably. Keep the same token and run npm.cmd run deploy:cloudflare again, or retry with VPN/firewall disabled."
  }
  if ($LASTEXITCODE -ne 0) {
    throw "Worker deploy failed. Check that the API token has Workers Scripts edit permission."
  }
}

Write-Host ""
Write-Host "Done. Open the Worker URL printed above and go to /admin." -ForegroundColor Green
