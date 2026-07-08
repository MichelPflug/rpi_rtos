<#
  tools/hw_verify.ps1 -- HW-in-the-loop Serial-Marker-Poller (T1.17).

  Erfasst die UART-Ausgabe des RC-Images und asserted gegen die ZENTRALE Marker-Taxonomie
  (tools/rc_markers.ps1) -- dieselben Marker, die build.ps1 in QEMU prueft. Drei Quellen:

    -ComPort COM5        echte Pi4-HW ueber USB-Seriell-Adapter (115200 8N1). HW-only.
    -SerialFile <pfad>   eine wachsende Serial-Capture-Datei (z.B. QEMU -serial file:...);
                         verifiziert die Poll-/Assert-Logik am Schreibtisch gegen QEMU.
    -SelfTest            startet selbst QEMU raspi4b mit dem (login-faehigen) kernel8.img,
                         schreibt dessen Serial in eine Datei und pollt sie live -> beweist
                         den Poller ohne HW. Voraussetzung: kernel8.img ist ein -Release-/
                         -Login-Image (enthaelt RC-READY + login:), plus sd.img.

  Gepollt wird INKREMENTELL (keine feste "erst schlafen, dann einmal lesen"-Wartezeit): das
  Log wird fortlaufend gelesen, jeder Marker wird abgehakt, sobald er auftaucht, und bei
  RC-READY + allen Pflicht-Markern gruen (oder KERNEL-HALT / Timeout) endet der Lauf.
  Exit 0 = alle Marker gesehen, 1 = Timeout/fehlend/Halt.
#>
[CmdletBinding()]
param(
    [string]$ComPort,
    [string]$SerialFile,
    [switch]$SelfTest,
    [int]$BaudRate = 115200,
    [int]$TimeoutSec = 45,
    [string]$LogFile = 'hw_serial.txt',
    [switch]$ResetPrompt,     # HW: vor dem Poll zum manuellen Power-Cycle auffordern
    [string]$Image = '_build/kernel8.img',
    [string]$Sd = '_build/sd.img',
    [ValidateSet('rc', 'vk', 'vision')][string]$MarkerSet = 'rc'   # Flavor-Marker-Satz (T1.17-Erweiterung)
)

$ErrorActionPreference = 'Stop'
# Marker-Satz je Flavor waehlen (eine Quelle je Flavor; RC bleibt Default/unveraendert).
switch ($MarkerSet) {
    'vk'     { . (Join-Path $PSScriptRoot 'vk_markers.ps1');     $ReadyMarkers = $VkReadyMarkers;     $HaltPattern = $VkHaltPattern;     $Forbidden = $VkForbidden }
    'vision' { . (Join-Path $PSScriptRoot 'vision_markers.ps1'); $ReadyMarkers = $VisionReadyMarkers; $HaltPattern = $VisionHaltPattern; $Forbidden = $VisionForbidden }
    default  { . (Join-Path $PSScriptRoot 'rc_markers.ps1');     $ReadyMarkers = $RcReadyMarkers;     $HaltPattern = $RcHaltPattern;     $Forbidden = $RcForbidden }
}

# Markiert neu aufgetauchte Pflicht-Marker in $acc (idempotent) und meldet sie als [PASS].
function Update-Seen([string]$acc, [hashtable]$seen) {
    foreach ($m in $ReadyMarkers) {
        if (-not $seen[$m.Name] -and ($acc -match $m.Pattern)) {
            $seen[$m.Name] = $true
            Write-Host ("  [PASS] {0}" -f $m.Name) -ForegroundColor Green
        }
    }
}

# Kern-Poller: $ReadAll liefert bei jedem Aufruf den GESAMTEN bisher erfassten Text. Laeuft bis
# alle Pflicht-Marker gesehen sind, $RcHaltPattern auftaucht, oder $TimeoutSec abgelaufen ist.
function Invoke-Poll([scriptblock]$ReadAll) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $seen = @{}
    $acc = ''
    $halt = $false
    while ((Get-Date) -lt $deadline) {
        $acc = & $ReadAll
        if ($acc) {
            Update-Seen $acc $seen
            if ($acc -match $HaltPattern) { $halt = $true; break }
            if ($seen.Count -eq $ReadyMarkers.Count) { break }
        }
        Start-Sleep -Milliseconds 120
    }
    if ($acc) { Set-Content -Path $LogFile -Value $acc -NoNewline -Encoding Ascii }

    Write-Host ''
    $ok = $true
    if ($halt) { $ok = (Check 'Kein Fail-closed-Halt / FEHLER-Marker' $false) -and $ok }
    foreach ($m in $ReadyMarkers) {
        if (-not $seen[$m.Name]) { $ok = (Check ("FEHLT: " + $m.Name) $false) -and $ok }
    }
    # GREP-CLEAN nur pruefen, wenn ein Forbidden-Satz definiert ist (RC) UND der Boot vollstaendig war.
    if ($Forbidden.Count -gt 0 -and $seen.Count -eq $ReadyMarkers.Count) {
        $dirty = @($Forbidden | Where-Object { $acc -match [regex]::Escape($_) })
        $msg = 'grep-clean (kein Scaffolding/Backdoor im Serial-Log)'
        if ($dirty.Count) { $msg += ' -- GEFUNDEN: ' + ($dirty -join ', ') }
        $ok = (Check $msg ($dirty.Count -eq 0)) -and $ok
    }
    return $ok
}

function Check($name, $cond) {
    if ($cond) { Write-Host ("  [PASS] {0}" -f $name) -ForegroundColor Green; return $true }
    else       { Write-Host ("  [FAIL] {0}" -f $name) -ForegroundColor Red;   return $false }
}

# --- Quelle: wachsende Datei (SerialFile / SelfTest) ---
# Liest bei jedem Poll die seit Poller-START neu angehaengten Bytes (share ReadWrite, damit der
# Schreiber -- QEMU -- parallel weiterschreibt). Der Basis-Offset = Dateilaenge bei Reader-Erzeugung
# ist die FRISCHE-Garantie: zeigt man -SerialFile auf ein altes, KOMPLETTES Boot-Log (das nicht mehr
# waechst), liefert der Reader nichts Neues -> Marker matchen nie -> Timeout/FAIL statt falsch-gruen.
function New-FileReader([string]$path) {
    $base = if (Test-Path $path) { (Get-Item $path).Length } else { 0 }
    return {
        if (-not (Test-Path $path)) { return '' }
        try {
            $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
            try {
                $len = $fs.Length
                if ($len -le $base) { return '' }          # nichts seit Start angehaengt
                [void]$fs.Seek($base, 'Begin')
                $buf = New-Object byte[] ($len - $base)
                $n = $fs.Read($buf, 0, $buf.Length)
                return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
            } finally { $fs.Dispose() }
        } catch { return '' }
    }.GetNewClosure()
}

Remove-Item -Force -ErrorAction SilentlyContinue $LogFile

# ============================================================
if ($SelfTest) {
    $qemu = $null
    foreach ($c in @('qemu-system-aarch64')) {
        $g = Get-Command $c -ErrorAction SilentlyContinue
        if ($g) { $qemu = $g.Source; break }
        foreach ($p in @("$env:ProgramFiles\qemu\$c.exe")) { if (Test-Path $p) { $qemu = $p; break } }
        if ($qemu) { break }
    }
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }
    if (-not (Test-Path $Image)) { Write-Warning "$Image fehlt -- zuerst .\build.ps1 -Release bauen."; exit 1 }

    $ser = Join-Path $PSScriptRoot '..\..\hw_selftest_serial.txt'   # Repo-Wurzel (_build/tools/../..)
    Remove-Item -Force -ErrorAction SilentlyContinue $ser
    $reader = New-FileReader $ser   # Basis-Offset 0 (Datei geloescht) -> erfasst den ganzen Boot
    $sdargs = @()
    if (Test-Path $Sd) { $sdargs = @('-drive', "file=$Sd,if=sd,format=raw,cache=writethrough") }
    Write-Host "SelfTest: QEMU raspi4b startet ($Image)..." -ForegroundColor Cyan
    $proc = Start-Process -FilePath $qemu `
        -ArgumentList (@('-M', 'raspi4b', '-kernel', $Image) + $sdargs +
                       @('-device', 'usb-kbd', '-serial', "file:$ser", '-display', 'none')) `
        -PassThru -WindowStyle Hidden
    try {
        $ok = Invoke-Poll $reader
    } finally {
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    }
    Write-Host ''
    if ($ok) { Write-Host 'HW-Verify (SelfTest/QEMU): ALLE MARKER PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'HW-Verify (SelfTest/QEMU): FEHLER' -ForegroundColor Red; exit 1 }
}

if ($SerialFile) {
    Write-Host "Poll: wachsende Serial-Datei '$SerialFile'..." -ForegroundColor Cyan
    $ok = Invoke-Poll (New-FileReader $SerialFile)
    if ($ok) { Write-Host 'HW-Verify (Datei): ALLE MARKER PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'HW-Verify (Datei): FEHLER' -ForegroundColor Red; exit 1 }
}

if ($ComPort) {
    if ($ResetPrompt) {
        Read-Host "Pi4 jetzt einschalten/reset (COM $ComPort, ${BaudRate} 8N1), dann Enter" | Out-Null
    }
    Write-Host "Poll: echte HW an $ComPort ($BaudRate 8N1)..." -ForegroundColor Cyan
    $sp = New-Object System.IO.Ports.SerialPort $ComPort, $BaudRate, 'None', 8, 'One'
    $sp.ReadTimeout = 200
    $sp.NewLine = "`n"
    $script:combuf = ''
    $sp.Open()
    try {
        $reader = { try { $script:combuf += $sp.ReadExisting() } catch {}; return $script:combuf }
        $ok = Invoke-Poll $reader
    } finally {
        $sp.Close(); $sp.Dispose()
    }
    if ($ok) { Write-Host 'HW-Verify (COM): ALLE MARKER PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'HW-Verify (COM): FEHLER' -ForegroundColor Red; exit 1 }
}

Write-Host 'Nichts zu tun: -SelfTest, -SerialFile <pfad> oder -ComPort <port> angeben.' -ForegroundColor Yellow
exit 2
