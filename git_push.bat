@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
echo ========================================================
echo  GitHub: Push Changes
echo ========================================================
echo.

git add .
set "commit_msg="
set /p commit_msg="Enter commit message (or press Enter for 'Auto update'): "

if not defined commit_msg (
    set "commit_msg=Auto update"
)

git commit -m "!commit_msg!"
git push

echo.
echo ========================================================
echo  Done!
echo ========================================================
pause
