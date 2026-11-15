# SPDX-License-Identifier: MPL-2.0
# Instala ds5haptics y lo que necesita (usbip-win2 y HidHide).
# Installs ds5haptics and what it needs (usbip-win2 and HidHide).
#
# Si la carpeta "instaladores" trae los instaladores (respaldo), no hace falta internet.
# Si no, los descarga de las páginas oficiales de cada proyecto en GitHub.
$ErrorActionPreference = 'Continue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$dest = 'C:\ds5haptics'
$inst = Join-Path $root 'instaladores'
$needReboot = $false

$es = (Get-UICulture).TwoLetterISOLanguageName -eq 'es'
function T([string]$textEs, [string]$textEn) { if ($es) { $textEs } else { $textEn } }

Write-Host '=========================================='
Write-Host (T '  Instalación de ds5haptics' '  ds5haptics setup')
Write-Host '=========================================='
Write-Host ''
Write-Host (T 'Antes de seguir: el DualSense tiene que estar EMPAREJADO por Bluetooth en Windows' 'Before you continue: the DualSense must be PAIRED over Bluetooth in Windows')
Write-Host (T 'y ENCENDIDO (botón PS), sin cable.' 'and TURNED ON (PS button), with no cable.')
Read-Host (T 'Presioná Enter para continuar' 'Press Enter to continue')

$exe = Join-Path $root 'ds5haptics.exe'
if (-not (Test-Path $exe)) {
    Write-Host (T 'Falta ds5haptics.exe en esta carpeta. Bajá el zip de "Releases" en GitHub, o compilalo con build.bat.' `
                  'ds5haptics.exe is missing from this folder. Download the zip from "Releases" on GitHub, or build it with build.bat.') -ForegroundColor Red
    exit 1
}

function Find-Installer([string[]]$patterns) {
    if (-not (Test-Path $inst)) { return $null }
    foreach ($p in $patterns) {
        $f = Get-ChildItem $inst -File | Where-Object { $_.Name -like $p -and $_.Name -notmatch 'arm64' } | Select-Object -First 1
        if ($f) { return $f }
    }
    return $null
}

# Descarga el instalador de la última versión publicada en GitHub por el propio proyecto
function Get-Installer([string]$repo, [string[]]$patterns, [string]$label) {
    $ans = Read-Host (T "No está el instalador de $label. ¿Descargarlo ahora de github.com/$repo? [S/n]" `
                        "The $label installer is not here. Download it now from github.com/$repo? [Y/n]")
    if ($ans -match '^(n|no)$') { return $null }
    try {
        $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'ds5haptics' }
        $asset = $null
        foreach ($p in $patterns) {
            $asset = $rel.assets | Where-Object { $_.name -like $p -and $_.name -notmatch 'arm64|debug|symbols|pdb' } | Select-Object -First 1
            if ($asset) { break }
        }
        if (-not $asset) { throw (T 'no encontré un instalador para Windows x64' 'no Windows x64 installer found') }
        New-Item -ItemType Directory -Force $inst | Out-Null
        $out = Join-Path $inst $asset.name
        Write-Host (T "  Bajando $($asset.name) ($($rel.tag_name))..." "  Downloading $($asset.name) ($($rel.tag_name))...")
        Invoke-WebRequest -UseBasicParsing $asset.browser_download_url -OutFile $out
        return Get-Item $out
    } catch {
        Write-Host (T "  No se pudo descargar: $_" "  Download failed: $_") -ForegroundColor Yellow
        Write-Host (T "  Bajalo a mano de https://github.com/$repo/releases" "  Download it yourself from https://github.com/$repo/releases") -ForegroundColor Yellow
        return $null
    }
}

# 1) usbip-win2 (asistente: aceptar las opciones por defecto)
$usbipFound = (Get-Command usbip.exe -ErrorAction SilentlyContinue) -or
              (Test-Path "$env:ProgramFiles\USBip\usbip.exe") -or (Test-Path "$env:ProgramFiles\usbip-win2\usbip.exe")
if ($usbipFound) {
    Write-Host (T '[1/5] usbip-win2 ya está instalado.' '[1/5] usbip-win2 is already installed.')
} else {
    $f = Find-Installer @('*usbip*x64*.exe', '*usbip*.exe', 'USBip*.exe', '*usbip*.msi')
    if (-not $f) { $f = Get-Installer 'vadimgrn/usbip-win2' @('*x64*.exe', '*.exe', '*x64*.msi', '*.msi') 'usbip-win2' }
    if ($f) {
        Write-Host (T "[1/5] Instalando usbip-win2 ($($f.Name)). Aceptá las opciones por defecto en el asistente..." `
                      "[1/5] Installing usbip-win2 ($($f.Name)). Accept the default options in the wizard...")
        Start-Process -FilePath $f.FullName -Wait
        $needReboot = $true
    } else {
        Write-Host (T '[1/5] usbip-win2 no se instaló: sin él no hay mando virtual.' '[1/5] usbip-win2 was not installed: without it there is no virtual controller.') -ForegroundColor Yellow
    }
}

# 2) HidHide (instalación silenciosa)
function Find-HidHideCli {
    @("$env:ProgramFiles\Nefarius Software Solutions\HidHide\HidHideCLI.exe",
      "$env:ProgramFiles\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (Find-HidHideCli) {
    Write-Host (T '[2/5] HidHide ya está instalado.' '[2/5] HidHide is already installed.')
} else {
    $f = Find-Installer @('HidHide*x64*.exe', 'HidHide*.exe')
    if (-not $f) { $f = Get-Installer 'nefarius/HidHide' @('HidHide*x64*.exe', 'HidHide*.exe', '*.exe') 'HidHide' }
    if ($f) {
        Write-Host (T "[2/5] Instalando HidHide ($($f.Name))..." "[2/5] Installing HidHide ($($f.Name))...")
        Start-Process -FilePath $f.FullName -ArgumentList '/exenoui /qn /norestart' -Wait
        $needReboot = $true
    } else {
        Write-Host (T '[2/5] HidHide no se instaló: los juegos podrían ver dos mandos.' '[2/5] HidHide was not installed: games may see two controllers.') -ForegroundColor Yellow
    }
}

# 3) Copiar el programa a una carpeta fija (HidHide autoriza por ruta)
Write-Host (T "[3/5] Copiando el programa a $dest..." "[3/5] Copying the program to $dest...")
if (Test-Path "$dest\ds5haptics.exe") { & "$dest\ds5haptics.exe" stop | Out-Null; Start-Sleep -Seconds 3 }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item $exe -Destination $dest -Force
foreach ($extra in 'README.md', 'LICENSE', 'CREDITS.md', 'desinstalar.bat', 'configurar-hidhide.bat', 'perfiles.bat') {
    $p = Join-Path $root $extra
    if (Test-Path $p) { Copy-Item $p -Destination $dest -Force }
}

# 4) Configurar HidHide: autorizar el programa y ocultar el mando Bluetooth a los juegos
Write-Host (T '[4/5] Configurando HidHide...' '[4/5] Setting up HidHide...')
& "$dest\ds5haptics.exe" hidhide-setup
$hidhideOk = ($LASTEXITCODE -eq 0)

# 5) Arranque automático al iniciar sesión
Write-Host (T '[5/5] Activando el inicio con Windows...' '[5/5] Enabling start with Windows...')
& "$dest\ds5haptics.exe" autostart on

Write-Host ''
Write-Host '=========================================='
Write-Host (T '  Instalación terminada' '  Setup finished') -ForegroundColor Green
Write-Host '=========================================='
if ($needReboot) {
    Write-Host (T 'Se instalaron drivers: REINICIÁ la PC. Al volver, ds5haptics arranca solo.' `
                  'Drivers were installed: RESTART the PC. ds5haptics will start by itself.') -ForegroundColor Cyan
    if (-not $hidhideOk) {
        Write-Host (T "Si después los juegos ven dos mandos, ejecutá $dest\configurar-hidhide.bat con el mando prendido." `
                      "If games then see two controllers, run $dest\configurar-hidhide.bat with the controller on.") -ForegroundColor Yellow
    }
} else {
    # Sin drivers nuevos no hace falta reiniciar: se inicia ahora mismo
    & "$dest\ds5haptics.exe" background | Out-Null
    Write-Host (T 'ds5haptics ya está funcionando: buscá el ícono del mando junto al reloj.' `
                  'ds5haptics is running: look for the controller icon next to the clock.') -ForegroundColor Cyan
    if (-not $hidhideOk) {
        Write-Host (T "HidHide no quedó configurado del todo: con el mando prendido, ejecutá $dest\configurar-hidhide.bat" `
                      "HidHide is not fully set up: with the controller on, run $dest\configurar-hidhide.bat") -ForegroundColor Yellow
    }
}
Write-Host (T "Perfiles por juego: $dest\perfiles.bat    Quitar todo: $dest\desinstalar.bat" `
              "Per-game profiles: $dest\perfiles.bat    Remove everything: $dest\desinstalar.bat")
