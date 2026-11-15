@echo off
rem Abre los perfiles por juego en el Bloc de notas. / Opens per-game profiles in Notepad.
set "EXE=C:\ds5haptics\ds5haptics.exe"
if not exist "%EXE%" set "EXE=%~dp0ds5haptics.exe"
"%EXE%" config
