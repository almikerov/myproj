$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

function Exit-With-Pause {
    param([string]$Message)
    if ($Message) {
        Write-Host $Message -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "Press Enter to exit..." -ForegroundColor Yellow
    Read-Host
    exit 1
}

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

function Ensure-Wrangler {
    $wrangler = Join-Path $ProjectRoot "node_modules\.bin\wrangler.cmd"
    if (!(Test-Path -LiteralPath $wrangler)) {
        Write-Host "Installing local Wrangler..." -ForegroundColor Cyan
        npm.cmd install --cache (Join-Path $ProjectRoot ".npm-cache")
        if ($LASTEXITCODE -ne 0) {
            Exit-With-Pause "npm install failed."
        }
    }
    return $wrangler
}

try {
    Write-Host "Reading settings from config.ini..." -ForegroundColor Cyan
    $config = Read-Config (Join-Path $ProjectRoot "config.ini")

    $cfToken = $config["CLOUDFLARE_API_TOKEN"]
    $adminToken = $config["ADMIN_TOKEN"]
    $deviceToken = $config["DEVICE_BOOTSTRAP_TOKEN"]
    $proxyUrl = $config["PROXY_URL"]

    if ([string]::IsNullOrWhiteSpace($cfToken)) {
        $cfToken = $env:CLOUDFLARE_API_TOKEN
    }

    if ([string]::IsNullOrWhiteSpace($cfToken)) {
        Exit-With-Pause "Set CLOUDFLARE_API_TOKEN in config.ini or in the current PowerShell session."
    }
    if ([string]::IsNullOrWhiteSpace($adminToken)) {
        Exit-With-Pause "Set ADMIN_TOKEN in config.ini."
    }
    if ([string]::IsNullOrWhiteSpace($deviceToken)) {
        Exit-With-Pause "Set DEVICE_BOOTSTRAP_TOKEN in config.ini."
    }

    $env:CLOUDFLARE_API_TOKEN = $cfToken
    $env:XDG_CONFIG_HOME = Join-Path $ProjectRoot ".wrangler-config"
    $env:WRANGLER_LOG_PATH = Join-Path $ProjectRoot ".wrangler-logs"
    $env:npm_config_cache = Join-Path $ProjectRoot ".npm-cache"

    if (![string]::IsNullOrWhiteSpace($proxyUrl)) {
        Write-Host "Using proxy: $proxyUrl" -ForegroundColor Cyan
        $env:HTTP_PROXY = $proxyUrl
        $env:HTTPS_PROXY = $proxyUrl
    } else {
        Remove-Item Env:HTTP_PROXY -ErrorAction SilentlyContinue
        Remove-Item Env:HTTPS_PROXY -ErrorAction SilentlyContinue
    }

    $wrangler = Ensure-Wrangler

    Write-Host "Checking Worker syntax..." -ForegroundColor Cyan
    node --check .\cloudflare\worker.js
    if ($LASTEXITCODE -ne 0) {
        Exit-With-Pause "Worker syntax check failed."
    }

    Write-Host "Syncing worker.txt from cloudflare/worker.js..." -ForegroundColor Cyan
    Copy-Item -LiteralPath .\cloudflare\worker.js -Destination .\worker.txt -Force

    Write-Host "Deploying Worker..." -ForegroundColor Cyan
    & $wrangler deploy --keep-vars
    if ($LASTEXITCODE -ne 0) {
        Exit-With-Pause "Worker deployment failed. Check the logs above."
    }

    Write-Host "Setting ADMIN_TOKEN..." -ForegroundColor Cyan
    $adminToken | & $wrangler secret put ADMIN_TOKEN
    if ($LASTEXITCODE -ne 0) {
        Exit-With-Pause "Failed to set ADMIN_TOKEN."
    }

    Write-Host "Setting DEVICE_BOOTSTRAP_TOKEN..." -ForegroundColor Cyan
    $deviceToken | & $wrangler secret put DEVICE_BOOTSTRAP_TOKEN
    if ($LASTEXITCODE -ne 0) {
        Exit-With-Pause "Failed to set DEVICE_BOOTSTRAP_TOKEN."
    }

    Write-Host ""
    Write-Host "Worker deployment completed successfully." -ForegroundColor Green
}
catch {
    Exit-With-Pause "Unexpected deployment error: $_"
}

Write-Host ""
Write-Host "Press Enter to exit..." -ForegroundColor Yellow
Read-Host
