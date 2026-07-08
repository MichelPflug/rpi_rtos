<#
  run_gui.ps1 -- die rpi_rtos WinForms-GUI (GUI.ELF) in QEMU zeigen und bedienen (T2.6).

  Baut das saubere Login->GUI-Image (-GuiLogin: interaktiver Login, KEIN Selbsttest-Rauschen)
  und startet QEMU raspi4b mit HDMI-Fenster + emulierter USB-Maus. Nach dem Login startet der
  Kernel GUI.ELF (Form mit Label, TextBox, Buttons) STATT der Shell -- genau das Ziel von Phase 2.

    .\run_gui.ps1              # bauen + starten (Login -> GUI); Tastatur im Terminal, Maus im Fenster
    .\run_gui.ps1 -WindowKbd   # USB-Tastatur statt Maus -> Login + GUI direkt IM FENSTER tippen (kein Cursor)
    .\run_gui.ps1 -SelfTest    # ohne Login: Selbsttest-Boot startet GUI.ELF direkt (mehr Boot-Ausgabe)
    .\run_gui.ps1 -NoDisplay   # ohne QEMU-Fenster (nur serielle Konsole)
    .\run_gui.ps1 -NoBuild     # vorhandenes kernel8.img/sd.img nutzen
    .\run_gui.ps1 -BuildOnly   # nur bauen

  --------------------------------------------------------------------------------------------
  ANMELDEN (im Terminal):   login: admin   password: admin   Neues Passwort: geheim42
                            (erzwungener Wechsel beim 1. Login; bleibt in sd.img erhalten)

  BEDIENEN -- die TASTATUR HIER IM TERMINAL ist der zuverlaessige Weg:
      Tippen   -> Text in die fokussierte TextBox (weisses Feld, gelber Fokusrahmen)
      Tab      -> Fokus zum naechsten Control (mit Umlauf)
      Enter    -> loest den fokussierten Button aus (z.B. +, Reset, Rot, Beenden)
  Die MAUS im HDMI-Fenster bewegt den Cursor und klickt Controls -- in QEMU emulationsbedingt
  aber nur SPORADISCH zuverlaessig (der dwc2-USB-HID liefert Reports nur schmal-fenstrig).
  Auf echter Pi-4-Hardware laeuft die Maus normal.

  BEENDEN:  in der GUI per Tab auf "Beenden" + Enter (bzw. anklicken) -- oder QEMU: Strg-A dann X.
  --------------------------------------------------------------------------------------------
#>
[CmdletBinding()]
param(
    [switch]$SelfTest,   # ohne Login: -GuiApp (Selbsttest-Boot startet GUI.ELF automatisch)
    [switch]$WindowKbd,  # USB-Tastatur ANstelle der Maus -> im HDMI-Fenster tippen (Login+GUI), aber ohne Cursor
    [switch]$NoDisplay,  # kein HDMI-Fenster (nur serielle Konsole)
    [switch]$NoBuild,    # vorhandenes Image verwenden
    [switch]$BuildOnly   # nur bauen, QEMU nicht starten
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

foreach ($dir in @('C:\Program Files\LLVM\bin', 'C:\Program Files\qemu')) {
    if ((Test-Path $dir) -and ($env:Path -notlike "*$dir*")) { $env:Path = "$dir;" + $env:Path }
}
function Find-Qemu {
    $g = Get-Command 'qemu-system-aarch64' -ErrorAction SilentlyContinue
    if ($g) { return $g.Source }
    $p = 'C:\Program Files\qemu\qemu-system-aarch64.exe'
    if (Test-Path $p) { return $p }
    return $null
}

# --- 1) Bauen ---
if (-not $NoBuild) {
    $flavor = if ($SelfTest) { '-GuiApp' } else { '-GuiLogin' }
    $mode   = if ($SelfTest) { 'Selbsttest-Boot startet GUI.ELF' } else { 'Login -> GUI-Sitzung' }
    Write-Host "Baue rpi_rtos (${flavor}: ${mode})..." -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') $flavor
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build fehlgeschlagen (Exit $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }
}

$img = Join-Path $root '_build/kernel8.img'
if (-not (Test-Path $img)) {
    Write-Host "kernel8.img fehlt -- einmal ohne -NoBuild starten." -ForegroundColor Red
    exit 1
}
if ($BuildOnly) {
    Write-Host "Build fertig: $img ($((Get-Item $img).Length) Bytes). QEMU nicht gestartet (-BuildOnly)." -ForegroundColor Green
    exit 0
}

# --- 2) QEMU mit HDMI-Fenster + USB-Maus starten ---
$qemu = Find-Qemu
if (-not $qemu) {
    Write-Host "qemu-system-aarch64 nicht gefunden (erwartet in PATH oder 'C:\Program Files\qemu')." -ForegroundColor Red
    exit 1
}

$sdargs = @()
$sd = Join-Path $root '_build/sd.img'
if (Test-Path $sd) {
    $sdargs = @('-drive', "file=$sd,if=sd,format=raw,cache=writethrough")
} else {
    Write-Host "Hinweis: keine sd.img gefunden -- einmal ohne -NoBuild bauen." -ForegroundColor Yellow
}

$displayArgs = if ($NoDisplay) { @('-display', 'none') } else { @('-display', 'gtk') }
# Eingabegeraet: default usb-mouse (Cursor + Klicks im Fenster; Tastatur ueber das Terminal).
# -WindowKbd: usb-kbd statt Maus -> im FENSTER tippen (Login + GUI-Navigation), kein Cursor.
# In BEIDEN Faellen bleibt die serielle Konsole (Terminal) ein zuverlaessiger Tastatur-Weg.
$hid = if ($WindowKbd) { 'usb-kbd' } else { 'usb-mouse' }
$qargs = @('-M', 'raspi4b', '-kernel', $img) + $sdargs + @('-device', $hid, '-serial', 'stdio') + $displayArgs

Write-Host ''
Write-Host '============================================================' -ForegroundColor DarkCyan
Write-Host '  rpi_rtos WinForms-GUI in QEMU (HDMI-Fenster + serielle Konsole)' -ForegroundColor Cyan
if (-not $SelfTest) {
    Write-Host '  1) Im TERMINAL anmelden:  admin / admin  -> neues Passwort (z.B. geheim42)' -ForegroundColor Gray
    Write-Host '  2) Danach startet die GUI-Sitzung automatisch im Fenster.' -ForegroundColor Gray
} else {
    Write-Host '  Selbsttest-Boot -> die GUI startet automatisch (mehr Boot-Ausgabe im Terminal).' -ForegroundColor Gray
}
if ($WindowKbd) {
    Write-Host '  BEDIENEN: Tastatur IM HDMI-FENSTER (USB-Tastatur) ODER hier im Terminal' -ForegroundColor Green
    Write-Host '     tippen = TextBox   Tab = naechstes Control   Enter = Button ausloesen' -ForegroundColor Gray
    Write-Host '     (Fenster-Eingabe in QEMU evtl. etwas zaeh -> Terminal ist der sichere Weg; kein Cursor)' -ForegroundColor DarkGray
} else {
    Write-Host '  BEDIENEN (zuverlaessig): Tastatur HIER IM TERMINAL' -ForegroundColor Green
    Write-Host '     tippen = TextBox   Tab = naechstes Control   Enter = Button ausloesen' -ForegroundColor Gray
    Write-Host '  Maus im Fenster: bewegt den Cursor/klickt -- in QEMU nur sporadisch (HW: normal).' -ForegroundColor DarkGray
    Write-Host '  Tipp: .\run_gui.ps1 -WindowKbd  -> stattdessen direkt im Fenster tippen (ohne Maus).' -ForegroundColor DarkGray
}
Write-Host '  Beenden: in der GUI per Tab auf "Beenden" + Enter -- oder QEMU: Strg-A dann X.' -ForegroundColor Gray
Write-Host '============================================================' -ForegroundColor DarkCyan
Write-Host ''

& $qemu @qargs
