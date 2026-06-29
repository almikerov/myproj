@echo off
echo Запуск деплоя Cloudflare...
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0scripts\deploy-cloudflare.ps1"
echo.
pause
