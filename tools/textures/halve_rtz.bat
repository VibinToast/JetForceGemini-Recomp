@echo off
rem Drag a texture pack .rtz onto this file: makes a half-size copy (4K -> 2K, 2K -> 1K).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0halve_rtz.ps1" "%~1"
