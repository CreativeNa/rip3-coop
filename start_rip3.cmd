@echo off
chcp 65001 >nul
title RIP 3 - mod
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_rip3.ps1"
if errorlevel 1 (
    echo.
    echo Launch failed. See build\launch.log
    pause
)
