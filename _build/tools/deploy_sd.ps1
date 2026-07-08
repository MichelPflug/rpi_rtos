<#
  tools/deploy_sd.ps1 -- Baut das kombinierte rpi_rtos-Image (Vulkan + GUI-Lib + KI-Vision + HDMI +
  Dev-Interface) und kopiert alle Dateien auf zwei bereits FAT32-formatierte SD-Partitionen:
      -Hdd0  (Boot/System, Default G:)  -> Firmware + config.txt + kernel8.img + System-ELFs
      -Hdd1  (User,        Default H:)  -> User-ELFs + Vulkan-.spv + Vision-Assets + Font

  Beispiel:
      .\tools\deploy_sd.ps1 -Firmware C:\rpi-firmware
      .\tools\deploy_sd.ps1 -Hdd0 G -Hdd1 H -Firmware C:\rpi-firmware -SkipBuild

  Sicherheit: das Image wird mit -DevRemote gebaut (ENTWICKLER-Fernsteuerung, UDP 192.168.10.244:5599).
  Das ist KEIN Produktions-Image. Plan: docs/architecture/20 (Dev-Remote), 19 (Vision), 17/18 (Vulkan).
#>
param(
    [string]  $Hdd0 = 'G',                                   # Laufwerksbuchstabe hdd0 (Boot/System)
    [string]  $Hdd1 = 'H',                                   # Laufwerksbuchstabe hdd1 (User)
    [string]  $Firmware = '',                                # Ordner mit Pi4-Firmware (start4.elf/fixup4.dat/DTB/armstub); leer = nur pruefen
    [string[]]$BuildFlags = @('-DevImage'),                 # Build-Flavor (kombiniertes Dev-Image)
    [switch]  $SkipBuild                                     # vorhandene Build-Artefakte wiederverwenden
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # Projektwurzel (_build/tools/../..)
Set-Location $root

function Fail($msg) { Write-Host "FEHLER: $msg" -ForegroundColor Red; exit 1 }
function Ok($msg)   { Write-Host "  [ok] $msg" -ForegroundColor Green }
function Info($msg) { Write-Host $msg -ForegroundColor Cyan }

# hdd0/hdd1 per Volume-LABEL aufloesen (RPIHDD0/RPIHDD1) -- ROBUST gegen verschobene Laufwerks-
# buchstaben (Windows vergibt sie beim Stecken/Ziehen anderer Medien neu!). Der -Hdd0/-Hdd1-Buchstabe
# dient nur als Fallback, falls kein passendes Label gefunden wird.
function Resolve-Drive($label, $hint) {
    $vol = Get-Volume -FileSystemLabel $label -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter } | Select-Object -First 1
    if ($vol) { return "$($vol.DriveLetter)" }
    $vol = Get-Volume -DriveLetter $hint -ErrorAction SilentlyContinue
    if ($vol) { return $hint }
    return $null
}
$d0 = Resolve-Drive 'RPIHDD0' $Hdd0
$d1 = Resolve-Drive 'RPIHDD1' $Hdd1
if (-not $d0) { Fail "hdd0 nicht gefunden (weder Label RPIHDD0 noch Laufwerk ${Hdd0}:). SD-Karte eingesteckt?" }
if (-not $d1) { Fail "hdd1 nicht gefunden (weder Label RPIHDD1 noch Laufwerk ${Hdd1}:). SD-Karte eingesteckt?" }
$g = "${d0}:\"
$h = "${d1}:\"

# --- 0) Ziel-Laufwerke pruefen (FAT32, per Label aufgeloest) --------------------------------------
Info "== Ziel-Laufwerke (per Volume-Label aufgeloest) =="
foreach ($pair in @(@{d=$d0;rolle='hdd0/Boot '}, @{d=$d1;rolle='hdd1/User '})) {
    $vol = Get-Volume -DriveLetter $pair.d -ErrorAction SilentlyContinue
    if (-not $vol) { Fail "Laufwerk $($pair.d): nicht gefunden." }
    if ($vol.FileSystem -ne 'FAT32') { Fail "Laufwerk $($pair.d): ist $($vol.FileSystem), erwartet FAT32." }
    Ok ("{0} -> {1}: {2} ({3}, {4:N1} GB)" -f $pair.rolle, $pair.d, $vol.FileSystemLabel, $vol.FileSystem, ($vol.Size / 1GB))
}

# --- 1) Bauen ------------------------------------------------------------------------------------
if (-not $SkipBuild) {
    Info "== Kombiniertes Image bauen: build.ps1 $($BuildFlags -join ' ') =="
    & (Join-Path $root 'build.ps1') @BuildFlags
    if ($LASTEXITCODE) { Fail "build.ps1 fehlgeschlagen." }
} else {
    Info "== -SkipBuild: vorhandene Artefakte verwenden =="
}
if (-not (Test-Path (Join-Path $root '_build/kernel8.img'))) { Fail "kernel8.img fehlt -- erst ohne -SkipBuild bauen." }

# --- 2) Erwartete Artefakte pruefen --------------------------------------------------------------
# Copy-Map: Quelle (relativ zur Wurzel) -> Zielname (8.3, wie der Kernel/die Apps sie oeffnen).
$hdd0Files = [ordered]@{
    '_build/kernel8.img' = 'kernel8.img'
    'boot/config.txt'= 'config.txt'
    '_build/hello.elf' = 'INIT.ELF'
    '_build/shell.elf' = 'SHELL.ELF'
    '_build/gui.elf'   = 'GUI.ELF'
    '_build/fptest.elf'= 'FPTEST.ELF'
    '_build/vktest.elf'= 'VKTEST.ELF'
    '_build/aivision.elf' = 'AIVISION.ELF'
}
$hdd1Files = [ordered]@{
    '_build/hello.elf'      = 'HELLO.ELF'
    '_build/guitest.elf'    = 'GUITEST.ELF'
    'user/gui_font.ttf'   = 'GUIFONT.TTF'
    '_build/fptest.elf'     = 'FPTEST.ELF'
    '_build/vkcube.elf'     = 'VKCUBE.ELF'
    'user/vk_vert.spv'    = 'VERT.SPV'
    'user/vk_frag.spv'    = 'FRAG.SPV'
    'user/vision_img.bmp' = 'VISIMG.BMP'
    'user/vision_net.net' = 'VISNET.NET'
    # Zusatzkopien, damit eine EL0-Shell (Policy: nur hdd1/hdd2) sie per 'run' starten kann:
    '_build/gui.elf'        = 'GUI.ELF'
    '_build/aivision.elf'   = 'AIVISION.ELF'
    '_build/vktest.elf'     = 'VKTEST.ELF'
}
$missing = @()
foreach ($src in ($hdd0Files.Keys + $hdd1Files.Keys | Select-Object -Unique)) {
    if (-not (Test-Path (Join-Path $root $src))) { $missing += $src }
}
if ($missing.Count) { Fail ("Build-Artefakte fehlen: {0}" -f ($missing -join ', ')) }

# --- 3) hdd0 (Boot/System) bespielen -------------------------------------------------------------
Info "== hdd0 -> ${g} (Boot/System) =="
foreach ($src in $hdd0Files.Keys) {
    Copy-Item -LiteralPath (Join-Path $root $src) -Destination (Join-Path $g $hdd0Files[$src]) -Force
    Ok ("{0} -> {1}{2}" -f $src, $g, $hdd0Files[$src])
}
Set-Content -Path (Join-Path $g 'SYSTEM.TXT') -Value "rpi_rtos System-Partition hdd0 (read-only gedacht).`r`n" -Encoding Ascii -NoNewline
Ok "SYSTEM.TXT erzeugt"

# --- 4) hdd1 (User) bespielen --------------------------------------------------------------------
Info "== hdd1 -> ${h} (User) =="
foreach ($src in $hdd1Files.Keys) {
    Copy-Item -LiteralPath (Join-Path $root $src) -Destination (Join-Path $h $hdd1Files[$src]) -Force
    Ok ("{0} -> {1}{2}" -f $src, $h, $hdd1Files[$src])
}
Set-Content -Path (Join-Path $h 'WELCOME.TXT') -Value "Willkommen auf der User-Partition hdd1 (rpi_rtos).`r`n" -Encoding Ascii -NoNewline
Ok "WELCOME.TXT erzeugt"

# --- 5) Firmware (nur echte HW; QEMU braucht sie nicht) ------------------------------------------
# Pflicht: start4.elf, fixup4.dat, bcm2711-rpi-4-b.dtb (der DTB schaltet erst den Dev-Agenten scharf,
# mmu_ram_from_dtb()). armstub8-gic.bin ist OPTIONAL -- enable_gic=1 nutzt den in start4.elf EINGEBAUTEN
# GIC-Armstub; die externe Datei ist nur ein Override. dtoverlay=disable-bt braucht overlays/.
Info "== Firmware (Pi4-Boot) =="
$fwNeeded    = @('start4.elf', 'fixup4.dat', 'bcm2711-rpi-4-b.dtb')
$fwOptional  = @('armstub8-gic.bin')
if ($Firmware) {
    if (-not (Test-Path $Firmware)) { Fail "Firmware-Ordner '$Firmware' nicht gefunden." }
    foreach ($f in $fwNeeded) {
        $src = Join-Path $Firmware $f
        if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $g $f) -Force; Ok "$f kopiert" }
        else { Write-Host "  [WARN] Pflicht-Firmware $f fehlt im Ordner '$Firmware'!" -ForegroundColor Red }
    }
    foreach ($f in $fwOptional) {
        $src = Join-Path $Firmware $f
        if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $g $f) -Force; Ok "$f kopiert (optionaler Override)" }
    }
    $ov = Join-Path $Firmware 'overlays'
    if (Test-Path $ov) { Copy-Item -LiteralPath $ov -Destination (Join-Path $g 'overlays') -Recurse -Force; Ok "overlays/ kopiert" }
    else { Write-Host "  [WARN] overlays/ fehlt -- dtoverlay=disable-bt (PL011 auf GPIO14/15) greift dann nicht." -ForegroundColor Yellow }
} else {
    $present = $fwNeeded | Where-Object { Test-Path (Join-Path $g $_) }
    if ($present.Count -eq $fwNeeded.Count) {
        Ok "Firmware bereits auf ${g} vorhanden"
    } else {
        Write-Host "  [WARN] Keine -Firmware angegeben und auf ${g} unvollstaendig." -ForegroundColor Yellow
        Write-Host "         Pflicht auf ${g} (z.B. aus einer Raspberry-Pi-OS-Boot-Partition):" -ForegroundColor Yellow
        $fwNeeded | ForEach-Object { Write-Host "           - $_" -ForegroundColor Yellow }
        Write-Host "         + Ordner overlays/  (armstub8-gic.bin optional, in start4.elf eingebaut)." -ForegroundColor Yellow
    }
}

# --- 6) Zusammenfassung --------------------------------------------------------------------------
Write-Host ''
Info "== Fertig =="
Write-Host ("  Kernel:      {0} Bytes" -f (Get-Item (Join-Path $root '_build/kernel8.img')).Length)
Write-Host  "  Dev-Interface: UDP 192.168.10.244:5599  (Host-Client: python _build/tools/dev_remote.py --host 192.168.10.244 ...)"
Write-Host  "  Login:       admin / admin  (Default; Passwortwechsel beim ersten Konsolen-Login erzwungen)"
Write-Host  "  Sicher auswerfen, in den Pi4 stecken, Ethernet ins 192.168.10.0/24-Netz, HDMI + USB-Tastatur."
