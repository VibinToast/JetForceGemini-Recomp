@echo off
rem Drag a large texture pack .rtz onto this file: splits it into parts under 2 GB for a GitHub release.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0split_rtz.ps1" "%~1"
