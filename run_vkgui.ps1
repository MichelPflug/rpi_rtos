# run_vkgui.ps1 -- Vulkan-Demo IN einer WinForms-GUI VISUELL starten.
#
#   .\run_vkgui.ps1            bauen (-VkGuiApp) + QEMU MIT Fenster: nach dem Login startet
#                             VKGUI.ELF -- eine WinForms-App mit eingebettetem, live gerendertem
#                             Vulkan-Wuerfel-Viewport (offscreen gerendert -> ins Panel geblittet).
#   .\run_vkgui.ps1 -NoBuild  nur starten (letztes Image verwenden)
#
# Bedienung im Fenster (USB-Maus): Buttons "Rotation an/aus", "bunt/warm/kuehl", "Beenden".
# Die Serial-Marker ([vkgui] ...) laufen parallel ueber dieses Terminal.
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
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') -VkGuiApp
    if ($LASTEXITCODE) { Write-Host 'Build fehlgeschlagen.' -ForegroundColor Red; exit 1 }
}

$qemu = Get-Command qemu-system-aarch64 -ErrorAction SilentlyContinue
if (-not $qemu) { Write-Host 'qemu-system-aarch64 nicht gefunden.' -ForegroundColor Red; exit 1 }

$sdargs = @()
if (Test-Path '_build/sd.img') { $sdargs = @('-drive', 'file=_build/sd.img,if=sd,format=raw') }

Write-Host 'Starte QEMU (Fenster): WinForms-Fenster mit rotierendem Vulkan-Wuerfel im Panel.' -ForegroundColor Cyan
Write-Host 'Bedienung mit der Maus (Buttons rechts). Beenden: "Beenden"-Button / Fenster schliessen.' -ForegroundColor Cyan
& qemu-system-aarch64 -M raspi4b -kernel _build/kernel8.img @sdargs -device usb-mouse -serial stdio -display gtk
