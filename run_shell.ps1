<#
  run_shell.ps1 -- Interaktive rpi_rtos-Shell in QEMU (ohne HDMI/Display).

  Baut das login-faehige Kernel-Image + die SD-Karte und startet QEMU raspi4b so, dass die
  serielle Konsole direkt in DIESEM Terminal liegt -- du bekommst einen `login:`-Prompt und
  danach eine bedienbare Shell. Kein HDMI-Kabel, kein QEMU-Fenster noetig.

    .\run_shell.ps1                # bauen (-Login) + interaktiv starten (nur serielle Konsole)
    .\run_shell.ps1 -Hdmi          # zusaetzlich das emulierte HDMI-Bild in einem QEMU-Fenster zeigen
    .\run_shell.ps1 -Hdmi -UsbKbd  # HDMI-Fenster + im Fenster tippen (emulierte USB-Tastatur)
    .\run_shell.ps1 -Release       # sauberes Produktions-Image (weniger Boot-Diagnose)
    .\run_shell.ps1 -NoBuild       # nicht neu bauen, vorhandenes kernel8.img/sd.img nutzen
    .\run_shell.ps1 -BuildOnly     # nur bauen, QEMU nicht starten

  -Hdmi oeffnet den VideoCore-Framebuffer (den der Kernel per Mailbox einrichtet, 640x480) in
  einem QEMU-Grafikfenster -- das ist das gleiche Bild, das auf echter HW am HDMI-Port erschiene.
  Die Ausgabe wird gespiegelt: du siehst sie im Fenster UND im Terminal. Getippt wird weiterhin
  ZUVERLAESSIG im Terminal (serielle Konsole). Mit zusaetzlich -UsbKbd haengt eine emulierte
  USB-Tastatur dran, dann kannst du auch direkt IM FENSTER tippen (in QEMU manchmal etwas zaeh --
  die serielle Eingabe im Terminal bleibt der sichere Weg).

  ---------------------------------------------------------------------------------------------
  ANMELDEN
    login:     admin
    password:  admin
    Neues Passwort:  (erzwungener Wechsel beim 1. Login) -> z.B.  geheim42
                     (mind. 4 Zeichen, nicht 'admin'; das neue Passwort bleibt in sd.img erhalten,
                      solange du nicht neu baust -> danach direkt mit -NoBuild + neuem Passwort rein)

  SHELL (Beispiele):  help  ls  pwd  whoami  cat hdd0:SYSTEM.TXT  cd hdd1:DOCS  passwd <neu>  exit

  BEENDEN von QEMU:   Strg-A loslassen, dann  X     (QEMU-Escape-Sequenz)
  ---------------------------------------------------------------------------------------------
#>
[CmdletBinding()]
param(
    [switch]$Release,    # grep-clean Produktions-Image statt -Login (weniger Boot-Ausgabe)
    [switch]$NoBuild,    # vorhandenes kernel8.img/sd.img verwenden, nicht neu bauen
    [switch]$BuildOnly,  # nur bauen, QEMU nicht starten
    [switch]$Hdmi,       # emuliertes HDMI-Bild in einem QEMU-Grafikfenster zeigen (statt -display none)
    [switch]$UsbKbd      # emulierte USB-Tastatur anhaengen -> Eingabe auch direkt im HDMI-Fenster
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

# Toolchain-/QEMU-Pfade ergaenzen (wie build.ps1), damit qemu-system-aarch64 gefunden wird.
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

# --- 1) Bauen (login-faehiges Image + SD-Karte) ---
if (-not $NoBuild) {
    $flavor = if ($Release) { '-Release' } else { '-Login' }
    Write-Host "Baue rpi_rtos (${flavor}: interaktiver Login + Shell)..." -ForegroundColor Cyan
    # In einem Kind-PowerShell bauen, damit ein eventuelles 'exit'/'throw' in build.ps1 dieses
    # Skript nicht mitbeendet; Exit-Code getrennt auswerten.
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

# --- 2) QEMU interaktiv ueber die serielle Konsole starten ---
$qemu = Find-Qemu
if (-not $qemu) {
    Write-Host "qemu-system-aarch64 nicht gefunden (erwartet in PATH oder 'C:\Program Files\qemu')." -ForegroundColor Red
    exit 1
}

$sdargs = @()
$sd = Join-Path $root '_build/sd.img'
if (Test-Path $sd) {
    # writethrough: ein per 'passwd'/Passwort-Wechsel geaendertes Passwort ueberlebt den Neustart.
    $sdargs = @('-drive', "file=$sd,if=sd,format=raw,cache=writethrough")
} else {
    Write-Host "Hinweis: keine sd.img gefunden -> Login/Shell brauchen sie. Einmal ohne -NoBuild bauen." -ForegroundColor Yellow
}

# -Hdmi -> QEMU-Grafikfenster (VideoCore-Framebuffer/HDMI); sonst headless. Eingabe immer
# zuverlaessig ueber die serielle Konsole (stdio); -UsbKbd erlaubt zusaetzlich Tippen im Fenster.
$displayArgs = if ($Hdmi) { @('-display', 'gtk') } else { @('-display', 'none') }
$kbdArgs     = if ($UsbKbd) { @('-device', 'usb-kbd') } else { @() }
$qargs = @('-M', 'raspi4b', '-kernel', $img) + $sdargs + $kbdArgs + @('-serial', 'stdio') + $displayArgs

Write-Host ''
Write-Host '============================================================' -ForegroundColor DarkCyan
if ($Hdmi) {
    Write-Host '  rpi_rtos-Shell in QEMU (HDMI-Fenster + serielle Konsole)'  -ForegroundColor Cyan
    Write-Host '  Das Grafikfenster zeigt das HDMI-Bild (640x480), sobald der Kernel' -ForegroundColor Gray
    Write-Host '  den Framebuffer eingerichtet hat. Getippt wird hier im Terminal' -ForegroundColor Gray
    if ($UsbKbd) { Write-Host '  ODER (Fenster fokussieren) direkt im HDMI-Fenster.' -ForegroundColor Gray }
} else {
    Write-Host '  rpi_rtos-Shell in QEMU (serielle Konsole, kein HDMI)'      -ForegroundColor Cyan
}
Write-Host '  Login:  admin / admin   -> neues Passwort setzen (z.B. geheim42)' -ForegroundColor Gray
Write-Host '  Beenden: Strg-A  dann  X   (oder das Fenster schliessen)'    -ForegroundColor Gray
Write-Host '============================================================' -ForegroundColor DarkCyan
Write-Host ''

# Vordergrund -> stdin/stdout des Terminals sind mit der seriellen Konsole verbunden (interaktiv).
& $qemu @qargs
