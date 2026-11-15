@echo off
setlocal
cd /d "%~dp0"
echo ==========================================
echo   Preparar respaldo completo de ds5haptics
echo ==========================================
echo.
echo Baja (una sola vez) los instaladores de usbip-win2 y HidHide y el codigo de libopus,
echo y arma ds5haptics-respaldo.zip con todo, incluido tu ds5haptics.exe ya compilado.
echo Necesita internet. Despues de formatear, ese zip alcanza para instalar todo sin internet.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\preparar-respaldo.ps1"
echo.
pause
