@echo off
rem Makes the portable zip for a release. See tools\package_release.ps1. Optional: package_release.bat 1.0.0
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_release.ps1" %*
pause
