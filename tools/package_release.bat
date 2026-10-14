@echo off
rem Asks for the version, builds the game and makes the portable zip for a release. See tools\package_release.ps1.
rem Optional: package_release.bat 1.0.1   (no question)   or   package_release.bat 1.0.1 -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package_release.ps1" %*
pause
