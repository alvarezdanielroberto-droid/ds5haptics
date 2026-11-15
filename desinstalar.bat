@echo off
setlocal
echo ==========================================
echo   Desinstalar ds5haptics / Uninstall ds5haptics
echo ==========================================
echo.
set "EXE=C:\ds5haptics\ds5haptics.exe"
if not exist "%EXE%" set "EXE=%~dp0ds5haptics.exe"
"%EXE%" stop
"%EXE%" autostart off
timeout /t 2 >nul
echo.
echo Se detuvo el programa y se quito el inicio con Windows.
echo The program was stopped and removed from Windows startup.
echo.
echo Para quitar los drivers, desinstala "USBip" y "HidHide" desde
echo Configuracion ^> Aplicaciones ^> Aplicaciones instaladas.
echo To remove the drivers, uninstall "USBip" and "HidHide" from
echo Settings ^> Apps ^> Installed apps.
echo.
echo La carpeta C:\ds5haptics se puede borrar a mano.
echo You can delete the C:\ds5haptics folder yourself.
echo.
pause
