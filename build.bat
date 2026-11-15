@echo off
setlocal
rem Siempre trabajar en la carpeta de este script (al ejecutar como admin, Windows arranca en System32).
cd /d "%~dp0"
echo ==========================================
echo   Compilando ds5haptics
echo ==========================================
echo.

rem 1) Si ya estamos en una consola de Visual Studio, usarla.
where cl >nul 2>nul
if not errorlevel 1 goto :msvc

rem 2) Buscar Visual Studio / Build Tools con vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :try_mingw
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :try_mingw
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto :try_mingw
echo Visual Studio encontrado en:
echo   %VSPATH%
echo.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto :try_mingw

:msvc
echo Compilador: MSVC (cl)
echo.
rem Con CMake se incluye el parlante del mando: descarga y compila libopus la primera vez.
where cmake >nul 2>nul
if errorlevel 1 goto :msvc_plain
echo Compilando con CMake (incluye el parlante; la primera vez descarga libopus)...
echo.
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DDS5_SPEAKER=ON -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
if errorlevel 1 goto :cmake_failed
cmake --build build
if errorlevel 1 goto :cmake_failed
copy /y build\ds5haptics.exe ds5haptics.exe >nul
if errorlevel 1 goto :fail
echo.
echo Compilado CON soporte de parlante (--speaker).
goto :ok

:cmake_failed
echo.
echo *** CMake fallo (sin internet para bajar libopus?). Compilo sin el parlante. ***
echo.

:msvc_plain
cl /nologo /std:c++17 /O2 /EHsc /MT /utf-8 /W4 /Isrc src\main_windows.cpp /Fe:ds5haptics.exe
if errorlevel 1 goto :fail
del /q main_windows.obj 2>nul
echo.
echo Compilado SIN soporte de parlante (falta CMake). Todo lo demas funciona igual.
echo Para agregarlo: Visual Studio Installer, Modificar, marcar "Herramientas de CMake de C++ para Windows".
goto :ok

rem 3) Si no hay Visual Studio, probar MinGW-w64 (g++) en el PATH.
:try_mingw
where g++ >nul 2>nul
if errorlevel 1 goto :nocompiler
echo Compilador: MinGW g++
echo.
g++ -std=c++17 -O2 -static -municode -Isrc src\main_windows.cpp -o ds5haptics.exe -lhid -lsetupapi -lwinmm -lole32 -lws2_32 -luser32 -ladvapi32 -lpsapi -lshell32 -lgdi32
if errorlevel 1 goto :fail
goto :ok

:nocompiler
echo No se encontro ningun compilador de C++ en esta PC.
echo.
echo Instala las herramientas de compilacion de Visual Studio (gratis).
echo Abri PowerShell y pega este comando:
echo.
echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo.
echo Son unos 2-3 GB. Cuando termine, volve a hacer doble clic en build.bat.
goto :end

:fail
echo.
echo *** La compilacion fallo. Copia todo el texto de esta ventana y pasaselo a Claude. ***
goto :end

:ok
echo.
echo ==========================================
echo   Listo: ds5haptics.exe
echo ==========================================
echo.
echo Para probarlo, abri una consola en esta carpeta (en el Explorador escribi cmd en la
echo barra de direcciones y Enter) y ejecuta:
echo   ds5haptics list
echo   ds5haptics info
echo   ds5haptics test pulses

:end
echo.
pause
