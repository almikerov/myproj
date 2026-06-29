@echo off
setlocal
cd /d "%~dp0"

echo ========================================================
echo  ESP32 Cloud Control - Automated Build and Deploy
echo ========================================================
echo.

echo --------------------------------------------------------
echo 1/2: Deploying Cloudflare Worker and Database...
echo --------------------------------------------------------
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0scripts\deploy-cloudflare.ps1"
if %errorlevel% neq 0 (
    echo [ERROR] Cloudflare deployment failed!
    pause
    exit /b %errorlevel%
)

echo.
echo --------------------------------------------------------
echo 2/2: Compiling and Uploading ESP32 Firmware...
echo --------------------------------------------------------
call build_and_upload.bat



echo.
echo ========================================================
echo  All additional components successfully built and deployed!
echo ========================================================
echo.
pause
