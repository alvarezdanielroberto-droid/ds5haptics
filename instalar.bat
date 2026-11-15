@echo off
setlocal
cd /d "%~dp0"
rem Pedir permisos de administrador (para instalar los drivers)
net session >nul 2>&1
if errorlevel 1 (
    echo Pidiendo permisos de administrador... / Asking for administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\instalar.ps1"
echo.
pause
