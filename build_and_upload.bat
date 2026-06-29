@echo off
setlocal
cd /d "%~dp0"
echo ========================================================
echo  ESP32 Firmware: Compile and Upload
echo ========================================================
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0scripts\upload-esp32.ps1" %*
echo.
pause
