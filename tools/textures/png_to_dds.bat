@echo off
rem Drag a texture pack folder onto this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0png_to_dds.ps1" "%~1"
