@echo off
rem Autoriza ds5haptics en HidHide y oculta el DualSense Bluetooth a los juegos.
rem Allows ds5haptics in HidHide and hides the Bluetooth DualSense from games.
setlocal
set "EXE=C:\ds5haptics\ds5haptics.exe"
if not exist "%EXE%" set "EXE=%~dp0ds5haptics.exe"
echo Prende el DualSense (boton PS) antes de seguir.
echo Turn on the DualSense (PS button) before continuing.
pause
"%EXE%" hidhide-setup
echo.
pause
