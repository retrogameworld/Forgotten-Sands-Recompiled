@echo off
rem Uploads this folder to GitHub: the code and README as a public repository, and
rem release\ForgottenSandsRecompiled-Windows-x64.zip as a GitHub Release download. No GitHub CLI needed - only Git for
rem Windows (git-scm.com), whose sign-in window opens the first time. Run "upload.bat check" to only run the checks.
if /i "%~1"=="check" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0upload.ps1" -DryRun
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0upload.ps1"
)
echo.
pause
