@echo off
rem Drag a texture pack folder onto this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_rtz.ps1" "%~1"
