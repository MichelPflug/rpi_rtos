# run_vk.ps1 -- Vulkan-/3D-Demo VISUELL starten (Phase 3, T3.6).
#
#   .\run_vk.ps1              bauen (-Vk) + QEMU MIT Fenster: Selbsttests laufen durch,
#                             danach dreht sich der VKCUBE (echte Vulkan-App) dauerhaft.
#   .\run_vk.ps1 -NoBuild     nur starten (letztes Image verwenden)
#
# Der Schirm gehoert der 3D-Ausgabe (fbcon-Handoff); die Selbsttest-Marker laufen
# parallel ueber die serielle Konsole (dieses Terminal).
[CmdletBinding()]
param(
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

foreach ($dir in @('C:\Program Files\LLVM\bin', 'C:\Program Files\qemu')) {
    if ((Test-Path $dir) -and ($env:Path -notlike "*$dir*")) { $env:Path = "$dir;" + $env:Path }
}

if (-not $NoBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -Vk
    if ($LASTEXITCODE) { Write-Host 'Build fehlgeschlagen.' -ForegroundColor Red; exit 1 }
}

$qemu = Get-Command qemu-system-aarch64 -ErrorAction SilentlyContinue
if (-not $qemu) { Write-Host 'qemu-system-aarch64 nicht gefunden.' -ForegroundColor Red; exit 1 }

$sdargs = @()
if (Test-Path '_build/sd.img') { $sdargs = @('-drive', 'file=_build/sd.img,if=sd,format=raw') }

Write-Host 'Starte QEMU (Fenster): erst Selbsttests, dann rotierender VKCUBE. Beenden: Fenster schliessen / Strg-A X.' -ForegroundColor Cyan
& qemu-system-aarch64 -M raspi4b -kernel _build/kernel8.img @sdargs -device usb-kbd -serial stdio
