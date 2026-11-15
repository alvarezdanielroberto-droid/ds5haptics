# Arma un respaldo completo y offline de ds5haptics.
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest es mucho más rápido sin barra
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$exe = Join-Path $root 'ds5haptics.exe'
if (-not (Test-Path $exe)) {
    Write-Host 'Falta ds5haptics.exe: primero compilalo con build.bat.' -ForegroundColor Red
    exit 1
}

$inst = Join-Path $root 'instaladores'
$deps = Join-Path $root 'deps'
New-Item -ItemType Directory -Force $inst, $deps | Out-Null

function Get-LatestAsset($repo, [string[]]$patterns) {
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'ds5haptics' }
    foreach ($p in $patterns) {
        $a = $rel.assets | Where-Object { $_.name -like $p -and $_.name -notmatch 'arm64|debug|symbols|pdb' } | Select-Object -First 1
        if ($a) { return @{ asset = $a; version = $rel.tag_name } }
    }
    return $null
}

function Save-Latest($repo, [string[]]$patterns, $label) {
    Write-Host "Buscando la última versión de $label..."
    $r = Get-LatestAsset $repo $patterns
    if (-not $r) { Write-Host "  No encontré un instalador de $label en GitHub ($repo)." -ForegroundColor Yellow; return $false }
    $out = Join-Path $inst $r.asset.name
    if (Test-Path $out) { Write-Host "  Ya estaba: $($r.asset.name)"; return $true }
    Write-Host "  Bajando $($r.asset.name) ($($r.version))..."
    Invoke-WebRequest -UseBasicParsing $r.asset.browser_download_url -OutFile $out
    return $true
}

$ok1 = Save-Latest 'vadimgrn/usbip-win2' @('*x64*.exe', '*.exe', '*x64*.msi', '*.msi') 'usbip-win2'
$ok2 = Save-Latest 'nefarius/HidHide' @('HidHide*x64*.exe', 'HidHide*.exe', '*.exe') 'HidHide'

$opus = Join-Path $deps 'opus-1.5.2.tar.gz'
if (-not (Test-Path $opus)) {
    Write-Host 'Bajando el código de libopus 1.5.2 (para recompilar sin internet)...'
    Invoke-WebRequest -UseBasicParsing 'https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz' -OutFile $opus
} else { Write-Host 'libopus ya estaba en deps\.' }

# Armar el zip: copia limpia (sin la carpeta build ni registros) dentro de una carpeta "ds5haptics"
$stage = Join-Path $env:TEMP 'ds5haptics-respaldo'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$dst = Join-Path $stage 'ds5haptics'
New-Item -ItemType Directory -Force $dst | Out-Null
Get-ChildItem $root -Force | Where-Object { $_.Name -notin @('build', '.git') -and $_.Extension -ne '.log' -and $_.Extension -ne '.obj' } |
    ForEach-Object { Copy-Item $_.FullName -Destination $dst -Recurse -Force }

$zip = Join-Path (Split-Path -Parent $root) 'ds5haptics-respaldo.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $dst -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Write-Host ''
Write-Host "Respaldo listo: $zip" -ForegroundColor Green
if (-not ($ok1 -and $ok2)) {
    Write-Host 'Ojo: faltó algún instalador (ver arriba). Bajalo a mano a la carpeta instaladores\ y volvé a ejecutar esto.' -ForegroundColor Yellow
}
Write-Host 'Guardalo en un pendrive o en la nube. Después de formatear: descomprimilo y ejecutá instalar.bat.'
