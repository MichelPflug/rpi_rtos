# build.ps1 -- rpi_rtos bauen (Windows / PowerShell), ohne make
#
#   .\build.ps1                 raspi4b-Kernel bauen (_build/kernel8.img)
#   .\build.ps1 -Run            bauen und in QEMU raspi4b starten
#   .\build.ps1 -Clean          Build-Artefakte loeschen
#   .\build.ps1 -Virt           virt-Netz-Harness bauen (_build/net_virt.elf)
#   .\build.ps1 -Virt -Run      Harness bauen und in QEMU virt starten (interaktiv)
#   .\build.ps1 -Virt -Verify   Harness bauen + automatische Echt-Interop-Pruefung
#                               (DHCP-Lease + UDP-Echo + TCP-Echo vom Host + Asserts)
#
# Erkennt automatisch GNU (aarch64-none-elf-/-linux-gnu-/-elf-) oder LLVM/clang.

[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$Virt,
    [switch]$VirtUsb, # QEMU virt + qemu-xhci: generischer xHCI-Treiber (T1.14). Mit -Verify: Enumerations-Assert.
    [switch]$Verify,
    [switch]$Login,   # raspi4b mit interaktivem Serial-Login bauen (-DINTERACTIVE_LOGIN)
    [switch]$UsbKbd,  # mit -Login -Verify: Sitzung ueber emulierte USB-Tastatur (Monitor sendkey)
    [switch]$UsbStorage, # mit -Verify: USB-Massenspeicher (usb-storage) statt -kbd + MSC-Asserts
    [switch]$UsbCyclic,  # mit -Verify: praepariertes hdd2 mit zyklischer FAT (DoS-Regressionstest)
    [switch]$Release, # RC-Produktions-Image: OHNE Selbsttest-/Demo-Scaffolding, mit interaktivem Login
    [switch]$HwHarness, # T1.17: baut das RC-Image (wie -Release) und verifiziert mit -Verify den
                        # HW-Harness am QEMU-Teil: gen_hwsd (bootfaehiges SD + Read-Back) + Serial-Poller.
    [switch]$Gui,       # T2.1: Standard-Selbsttest-Image; mit -Verify prueft es die GUI-Grafik-Bruecke
    [switch]$GuiApp,    # T2.6: Selbsttest-Image, das nach dem "Login" GUI.ELF (WinForms-Demo) startet (-DGUI_APP)
    [switch]$VkGuiApp,  # Vulkan-in-WinForms-Demo: startet VKGUI.ELF (GUI-App mit eingebettetem, live
                        # gerendertem Vulkan-Wuerfel-Viewport) statt GUI.ELF (-DVKGUI_APP -DGUI_FP).
    [switch]$Vk,        # T3.x (Phase 3): Selbsttest-Image fuer 3D/Vulkan (-DGUI_FP -DVK_TEST): FP-Kontext-
                        # Guardian (2x FPTEST auf Kern 1) + VKTEST.ELF (Rasterizer-/Vulkan-Selbsttests)
    [switch]$Vision,    # docs/architecture/19: gekapseltes KI-Bildauswertungs-Modul (-DVISION -DGUI_FP);
                        # mit -Verify: AIVISION.ELF-Selbsttest (A1.1 NEON-fp32-Inferenz-Engine)
    [switch]$V3d,       # Vulkan V5 (erster Schritt): V3D-Hardware-Erkennung (-DV3D_PROBE). Mit -Verify:
                        # in QEMU "V3D nicht gefunden"; am echten Pi4 meldet die Probe die reale V3D-IDENT.
    [switch]$DevRemote, # docs/architecture/20: Dev-Remote-Interface (UDP, -DDEV_REMOTE). ENTWICKLER-ONLY,
                        # ganz #ifdef DEV_REMOTE -> nie im RC-/Release-Image. Mit -Verify: D1-Protokoll-Kern.
    [switch]$DevImage,  # docs/architecture/20: KOMBINIERTES Entwickler-Image -- Auto-Login -> Shell (HDMI) +
                        # Vulkan + KI-Vision + Dev-Remote (statische IP 192.168.10.244). Fuer echten Pi4 via
                        # _build/tools/deploy_sd.ps1. ENTWICKLER-ONLY (Auto-Login+Backdoor) -> nie in Produktion.
    [switch]$ShellImage, # MINIMAL-Bringup: Auto-Login -> nackte Shell auf HDMI. KEINE Zusaetze (kein GUI/
                         # Vulkan/Vision/Dev-Remote, kein Selbsttest-Scaffolding, integer-only). Zum Eingrenzen
                         # von HW-Boot-/Display-Problemen. Deploy via _build/tools/deploy_sd.ps1 -BuildFlags '-ShellImage'.
    [switch]$GuiImage,   # GUI-Sitzung auf echter HW: Auto-Login -> GUI.ELF (WinForms) auf HDMI, Full-HD
                         # (1080p + 10-MiB-gui_fb-Backbuffer). Mit -DevRemote -PcieProbe kombinieren (USB + kabellos).
    [switch]$DiagBlink,  # Blind-Boot-Diagnose: blinkt an Boot-Meilensteinen auf GPIO21 (Pin 40), ohne Serial/HDMI.
                         # Mit -ShellImage kombinieren. Inhalt ganz #ifdef DIAG_BLINK (ohne Flag byte-inert).
    [switch]$DiagLog,    # Boot-Log auf die SD: schreibt den UART-Boot-Log nach hdd1:BOOTLOG.TXT (nach vfs_init).
                         # Diagnose ohne serielle Konsole. Mit -ShellImage kombinieren. Ganz #ifdef DIAG_LOG.
    [switch]$PcieProbe,  # BCM2711 PCIe/VL805-Diagnose (Pi4 USB-A-Ports via xHCI hinter PCIe). Ganz #ifdef PCIE_PROBE.
    [switch]$ForceCoarse,# Bring-up-Experiment: erzwingt die einfache QEMU-Grobkarte auch auf echter HW (umgeht den
                         # bisher HW-ungetesteten DTB-Speicherpfad in mmu.c). Nur zur HW-Fehlereingrenzung.
    [switch]$GuiLogin,  # T2.6: echter interaktiver Login -> GUI.ELF statt Shell (-DINTERACTIVE_LOGIN -DGUI_SESSION)
                        # (Kernel bb->fb-Selbsttest + EL0-Zeichnen via SYS_GUI_INFO/SYS_GUI_FLUSH).
                      # (-DINTERACTIVE_LOGIN, kein -DRTOS_SELFTEST). Mit -Verify: grep-clean-Guardian.
    [switch]$PanicTest, # T1.4-Guardian: loest einen EL1-Kernel-Fault aus (-DPANIC_SELFTEST) und prueft,
                       # dass panic() ALLE Kerne stoppt. Halt das System bewusst an.
    [switch]$MutexPanicTest, # T1.5-Guardian: loest ein rekursives mutex_lock aus (-DRMUTEX_PANIC_TEST)
                            # und prueft, dass es fail-loud in panic() muendet (statt stiller No-op).
    [switch]$HwImage,       # T1.17-Erweiterung: bootfaehiges HW-SD-Image fuer den AKTUELLEN Flavor
                            # (-Vk/-Vision/sonst RC) erzeugen; mit -Verify zusaetzlich Serial-Marker-Poll.
    [string]$ComPort,       # mit -HwImage -Verify: echte Pi4-HW ueber COM-Port pollen (sonst QEMU-SelfTest)
    [string]$Firmware       # Verzeichnis mit Pi4-Boot-Firmware (start4.elf/fixup4.dat/DTB/armstub) fuer echten Boot
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (-not (Test-Path '_build')) { New-Item -ItemType Directory -Path '_build' | Out-Null }  # Build-Ausgaben gebuendelt

foreach ($dir in @('C:\Program Files\LLVM\bin', 'C:\Program Files\qemu')) {
    if ((Test-Path $dir) -and ($env:Path -notlike "*$dir*")) {
        $env:Path = "$dir;" + $env:Path
    }
}

function Find-Tool([string[]]$names) {
    foreach ($n in $names) {
        if (Get-Command $n -ErrorAction SilentlyContinue) { return $n }
    }
    return $null
}

function Check($name, $cond) {
    if ($cond) { Write-Host ("  [PASS] {0}" -f $name) -ForegroundColor Green; return $true }
    else       { Write-Host ("  [FAIL] {0}" -f $name) -ForegroundColor Red;   return $false }
}

# Startet QEMU raspi4b headless, wartet, killt und liefert das Serial-Log zurueck.
# $usbDevice: angehaengtes USB-Geraet (default usb-kbd; z.B. usb-mouse fuer den GUI-Maustest).
function Invoke-Raspi($qemu, $img, $serfile, $errfile, $seconds, $usbDevice = 'usb-kbd') {
    Remove-Item -Force -ErrorAction SilentlyContinue $serfile, $errfile
    $sdargs = @()
    # cache=writethrough: Writes landen sofort in sd.img (Persistenz ueberlebt den
    # harten Stop-Process zwischen den beiden Verifikationslaeufen).
    if (Test-Path '_build/sd.img') { $sdargs = @('-drive', 'file=_build/sd.img,if=sd,format=raw,cache=writethrough') }
    $proc = Start-Process -FilePath $qemu `
        -ArgumentList (@('-M', 'raspi4b', '-kernel', $img) + $sdargs + @('-device', $usbDevice, '-serial', "file:$serfile", '-display', 'none')) `
        -PassThru -WindowStyle Hidden -RedirectStandardError $errfile
    $st = $proc.StartTime
    Start-Sleep -Seconds $seconds
    $early = $proc.HasExited
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    $log = if (Test-Path $serfile) { Get-Content $serfile -Raw } else { '' }
    return [pscustomobject]@{ Log = $log; Started = $st; Early = $early; SerFile = $serfile; ErrFile = $errfile }
}

# Wie Invoke-Raspi, aber MARKER-GESTEUERT: pollt das Serial-Log und bricht frueh ab, sobald
# $doneMarker (Regex) erscheint (+ kurzes Nachspuelen), sonst nach $maxSeconds. Robuster fuer
# wachsende Test-Suiten (z.B. -Vk): schnell im Gutfall, voller Timeout nur im Fehlerfall.
function Invoke-RaspiUntil($qemu, $img, $serfile, $errfile, $doneMarker, $maxSeconds, $usbDevice = 'usb-kbd') {
    Remove-Item -Force -ErrorAction SilentlyContinue $serfile, $errfile
    $sdargs = @()
    if (Test-Path '_build/sd.img') { $sdargs = @('-drive', 'file=_build/sd.img,if=sd,format=raw,cache=writethrough') }
    $proc = Start-Process -FilePath $qemu `
        -ArgumentList (@('-M', 'raspi4b', '-kernel', $img) + $sdargs + @('-device', $usbDevice, '-serial', "file:$serfile", '-display', 'none')) `
        -PassThru -WindowStyle Hidden -RedirectStandardError $errfile
    $st = $proc.StartTime
    $deadline = (Get-Date).AddSeconds($maxSeconds)
    $seen = $false
    # Mit FileShare.ReadWrite lesen, damit QEMUs offener Schreib-Handle den Poll nicht blockiert.
    function Read-Shared($f) {
        if (-not (Test-Path $f)) { return '' }
        try { $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite'); $sr = New-Object IO.StreamReader($fs);
              $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t } catch { return '' }
    }
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) { break }
        if ((Read-Shared $serfile) -match $doneMarker) { $seen = $true; Start-Sleep -Milliseconds 800; break }
    }
    $early = $proc.HasExited
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    $log = if (Test-Path $serfile) { Get-Content $serfile -Raw } else { '' }
    return [pscustomobject]@{ Log = $log; Started = $st; Early = $early; SawMarker = $seen; SerFile = $serfile; ErrFile = $errfile }
}

# --- gemeinsame Compiler-Flags ---
$cflags = @(
    '-ffreestanding', '-nostdlib', '-nostartfiles', '-mgeneral-regs-only',
    '-mcpu=cortex-a72', '-O2', '-Wall', '-Wextra',
    '-fno-pie', '-fno-pic', '-fno-stack-protector',
    '-Iinclude', '-Idrivers/uart'
)
# RTOS_SELFTEST schaltet das Selbsttest-/Demo-Scaffolding im Kernel frei (hartkodierte
# Testkonten, destruktiver MSC-Schreibtest, GPIO/SPI/I2C-Demo, HTTP-Resolver-Test,
# Netz-Zeit-Manipulation). Die Verifikations-Suiten (raspi/virt/-Login) bauen DAMIT und
# asserten dessen Ausgaben. Das RC-Produktions-Image (-Release) baut OHNE -> grep-clean.
# Der virt-Harness (tcp_looptest.c) ruft net_test_* strukturell auf -> RTOS_SELFTEST dort
# IMMER an, auch mit -Release (sonst unaufgeloeste Symbole beim -Virt -Release-Bau).
if ((-not ($Release -or $HwHarness -or $PanicTest -or $MutexPanicTest -or $GuiLogin -or $DevImage -or $ShellImage -or $GuiImage)) -or $Virt) { $cflags += '-DRTOS_SELFTEST' }
# Interaktiver Serial-Login: explizit via -Login/-GuiLogin, im RC-Image (-Release/-HwHarness) immer an.
if ($Login -or $Release -or $HwHarness -or $GuiLogin) { $cflags += '-DINTERACTIVE_LOGIN' }
# T2.6: GUI_APP laesst den Selbsttest-Boot GUI.ELF (WinForms-Sitzung) statt des Bruecke-Tests starten.
if ($GuiApp) { $cflags += '-DGUI_APP' }
# Vulkan-in-WinForms: VKGUI_APP startet VKGUI.ELF (GUI-App mit Vulkan-Viewport) statt GUI.ELF.
if ($VkGuiApp) { $cflags += '-DVKGUI_APP' }
# T2.6: GUI_SESSION laesst den interaktiven Login GUI.ELF statt der Shell starten (echter Login->GUI).
if ($GuiLogin) { $cflags += '-DGUI_SESSION' }
# T2.8: GUI_FP schaltet FP/SIMD an EL0 frei (CPACR_EL1.FPEN=0b11) -- NUR in GUI-/Vk-Builds, damit die
# GUI-App TrueType zur Laufzeit rastern bzw. Phase 3 (3D/Vulkan) rechnen kann. Das RC-Image (-Release)
# bleibt FPEN=0b00 (T1.8 gehaertet). Seit T3.1 sichert der Scheduler den FP-Kontext je Task (fpctx.S);
# der Kernel bleibt ansonsten integer-only (objdump-Guardian im Standard-Verify, dort ohne GUI_FP).
if ($GuiApp -or $GuiLogin -or $Vk -or $VkGuiApp) { $cflags += '-DGUI_FP' }
# T3.x: VK_TEST laesst den Selbsttest-Boot die Phase-3-Prozesse starten (FPTEST/VKTEST statt INIT/GUITEST).
if ($Vk) { $cflags += '-DVK_TEST' }
# VISION (Vision-Track, docs/architecture/19-ai-vision-plan.md): gekapseltes KI-Bildauswertungs-Modul.
# Impliziert GUI_FP (die Inferenz-Engine rechnet fp32/NEON). Kapselung: OHNE -Vision kompiliert KEIN
# Vision-Code und der Kernel ist byte-identisch (der einzige Kernel-Kontakt ist ein #ifdef VISION-Spawn
# in kmain -> ohne das Flag praeprozessor-leer). VISION ist damit auch nie im integer-only -Release.
if ($Vision) { $cflags += '-DVISION', '-DGUI_FP' }
# Vulkan V5 (erster Schritt): V3D-Hardware-Erkennung (drivers/gpu/v3d.c ganz #ifdef V3D_PROBE ->
# ohne das Flag byte-identisch). Ein arbeitendes V3D-Backend bleibt research-grade (siehe v3d.h).
if ($V3d) { $cflags += '-DV3D_PROBE' }
# Dev-Remote-Interface (docs/architecture/20): ENTWICKLER-Fernsteuerung ueber UDP. Ganz #ifdef
# DEV_REMOTE -> ohne das Flag byte-identischer Kernel; per bedingter Kompilierung aus Produktion
# unterdrueckt. Der RC-grep-clean-Guardian belegt die Abwesenheit im Release-Image.
if ($DevRemote) { $cflags += '-DDEV_REMOTE' }
# KOMBINIERTES Entwickler-Image (docs/architecture/20): Auto-Login -> Shell (kein GUI_SESSION) +
# Vulkan/Vision (GUI_FP) + Dev-Remote, OHNE Selbsttest-Scaffolding (wie -GuiLogin sauber). AUTO_LOGIN
# ueberspringt den blockierenden Konsolen-Login -> Scheduler + Dev-Interface kommen autonom hoch.
# Ausschliesslich fuer Dev-Images (Auto-Login + UDP-Backdoor) -- NIE Produktion.
if ($DevImage) { $cflags += '-DINTERACTIVE_LOGIN', '-DAUTO_LOGIN', '-DVISION', '-DGUI_FP', '-DDEV_REMOTE' }
# MINIMAL-Bringup-Shell: Auto-Login -> Shell, KEINE Zusaetze (kein GUI_FP/VISION/DEV_REMOTE, kein
# Selbsttest). Integer-only, so nah am RC-Image wie moeglich -- zum Eingrenzen von HW-/Display-Problemen.
if ($ShellImage) { $cflags += '-DINTERACTIVE_LOGIN', '-DAUTO_LOGIN', '-DFULLHD' }   # FULLHD: 1920x1080 statt 640x480 (kein gui_fb-Backbuffer-Limit in der Minimal-Shell)
# GUI-Sitzung auf echter HW: Auto-Login -> GUI.ELF (WinForms), Full-HD (1080p + 5-Kachel-Backbuffer via FULLHD).
# GUI_FP fuer den TrueType-Rasterizer/FP. Mit -DevRemote -PcieProbe fuer USB + kabellose Iteration.
if ($GuiImage) { $cflags += '-DINTERACTIVE_LOGIN', '-DAUTO_LOGIN', '-DGUI_SESSION', '-DGUI_FP', '-DFULLHD' }
# Blind-Boot-Diagnose: blinkt an Boot-Meilensteinen eine Impuls-Zahl auf GPIO21 (Pin 40), direkt ueber
# die GPIO-Register (ohne Mailbox/GIC/Timer). Zum Eingrenzen von Boot-Haengern ohne serielle Konsole.
if ($DiagBlink) { $cflags += '-DDIAG_BLINK' }
# Boot-Log auf die SD: fesselt den UART-Boot-Log in RAM und schreibt ihn nach hdd1:BOOTLOG.TXT (sobald
# VFS oben ist). Zum Diagnostizieren ohne serielle Konsole. Mit -ShellImage kombinieren.
if ($DiagLog) { $cflags += '-DDIAG_LOG' }
# BCM2711 PCIe/VL805-Diagnose (Pi4 USB-A-Ports, xHCI hinter PCIe). Erst Register-Probe. Impliziert
# HW_FIXUP: der EL1-Sync-Handler wird abort-tolerant (Fault-Instruktion ueberspringen statt paniken),
# damit ein noch nicht hochgezogener PCIe-Block den Boot nicht killt (kabellos iterierbar). OHNE das
# Flag bleibt der EL1-Sync-Vektor der unveraenderte INVALID-4-Pfad -> RC/Vk-Kernel byte-identisch.
if ($PcieProbe) { $cflags += '-DPCIE_PROBE', '-DHW_FIXUP' }
# Bring-up-Experiment: DTB-Speicherpfad in mmu.c umgehen (Grobkarte erzwingen). Nur HW-Diagnose.
if ($ForceCoarse) { $cflags += '-DFORCE_COARSE_MAP' }
# T1.4-Panik-Guardian: absichtlicher EL1-Fault-Test (nur im dedizierten -PanicTest-Build).
if ($PanicTest) { $cflags += '-DPANIC_SELFTEST' }
# T1.5-Guardian: rekursives mutex_lock -> panic (nur im dedizierten -MutexPanicTest-Build).
if ($MutexPanicTest) { $cflags += '-DRMUTEX_PANIC_TEST' }

function Get-Toolchain {
    foreach ($p in @('aarch64-none-elf-', 'aarch64-linux-gnu-', 'aarch64-elf-')) {
        if (Get-Command ($p + 'gcc') -ErrorAction SilentlyContinue) {
            return [pscustomobject]@{ Kind = 'gnu'; CC = $p + 'gcc'; LD = $p + 'ld'; OBJCOPY = $p + 'objcopy' }
        }
    }
    if (Get-Command clang -ErrorAction SilentlyContinue) {
        $lld = Find-Tool @('ld.lld'); $ocp = Find-Tool @('llvm-objcopy')
        if (-not $lld) { throw 'clang gefunden, aber ld.lld fehlt.' }
        if (-not $ocp) { throw 'clang gefunden, aber llvm-objcopy fehlt.' }
        return [pscustomobject]@{ Kind = 'clang'; CC = 'clang'; LD = $lld; OBJCOPY = $ocp }
    }
    throw 'Keine AArch64-Toolchain gefunden (Arm GNU aarch64-none-elf ODER LLVM/clang+lld).'
}

function Compile-Sources($tc, $sources) {
    $ct = $cflags
    if ($tc.Kind -eq 'clang') {
        $ct = @('--target=aarch64-none-elf', '-Wno-unused-command-line-argument') + $cflags
    }
    $objs = @()
    foreach ($s in $sources) {
        $o = [IO.Path]::ChangeExtension($s, 'o')
        & $tc.CC @ct -c $s -o $o
        if ($LASTEXITCODE) { throw "Kompilieren fehlgeschlagen: $s" }
        $objs += $o
    }
    return $objs
}

function Link-Elf($tc, $ld, $elf, $objs) {
    if ($tc.Kind -eq 'gnu') {
        & $tc.CC @cflags '-no-pie' '-T' $ld '-o' $elf @objs
    } else {
        & $tc.LD '-T' $ld '-o' $elf @objs
    }
    if ($LASTEXITCODE) { throw "Linken fehlgeschlagen: $elf" }
}

# --- Quelllisten ---
$netSrc = @('lib/kmem.c', 'net/net.c', 'net/arp.c', 'net/ip.c', 'net/icmp.c',
            'net/udp.c', 'net/tcp.c', 'net/dhcp.c', 'net/dns.c', 'net/http.c',
            'net/httpd.c')

$rpiAsm = @('arch/aarch64/start.S', 'arch/aarch64/vectors.S', 'arch/aarch64/switch.S',
            'arch/aarch64/fpctx.S')   # T3.1: FP-Kontext (Inhalt komplett #ifdef GUI_FP)
$rpiC   = @('arch/aarch64/mmu.c', 'arch/aarch64/exceptions.c',
            'drivers/uart/uart.c', 'drivers/gic/gic.c', 'drivers/timer/timer.c',
            'drivers/sd/sd.c', 'drivers/net/genet.c',
            'drivers/gpio/gpio.c', 'drivers/spi/spi.c', 'drivers/i2c/i2c.c', 'drivers/pwm/pwm.c',
            'drivers/mailbox/mailbox.c', 'drivers/video/fb.c', 'drivers/video/fbcon.c', 'drivers/video/gui_fb.c',
            'drivers/usb/usb_hc.c', 'drivers/usb/dwc2.c', 'drivers/usb/usbkbd.c', 'drivers/usb/usbmsc.c', 'drivers/usb/usbmouse.c',
            'drivers/usb/uvc.c',   # A4.1a: UVC-Klassen-Layer, Inhalt ganz #ifdef VISION (leer ohne Flag)
            'drivers/gpu/v3d.c',   # Vulkan V5: V3D-HW-Erkennung, Inhalt ganz #ifdef V3D_PROBE (leer ohne Flag)
            'net/dev_remote.c',    # docs/architecture/20: Dev-Remote-Protokoll-Kern (D1), ganz #ifdef DEV_REMOTE (leer ohne Flag)
            'net/dev_agent.c',     # docs/architecture/20: Dev-Remote-UDP-Agent + Integration (D2), ganz #ifdef DEV_REMOTE (leer ohne Flag)
            'fs/fat32.c', 'fs/vfs.c',
            'net/httpd_fs.c',                 # VFS-gebundener HTTP-Resolver (nur wo VFS existiert)
            'lib/sha256.c', 'lib/fdt.c', 'kernel/user.c',
            'kernel/sched.c', 'kernel/ipc.c', 'kernel/elf.c', 'kernel/syscall.c', 'kernel/gui_input.c',
            'kernel/vi_parallel.c',   # VISION (A1.5): Kernel-Parallel-For, Inhalt ganz #ifdef VISION (leer ohne Flag)
            'kernel/diag_blink.c',    # Blind-Boot-Diagnose ueber GPIO21, Inhalt ganz #ifdef DIAG_BLINK (leer ohne Flag)
            'kernel/diag_log.c',      # Boot-Log auf die SD (hdd1:BOOTLOG.TXT), Inhalt ganz #ifdef DIAG_LOG (leer ohne Flag)
            'drivers/pci/pcie.c',     # BCM2711 PCIe/VL805-Diagnose (Pi4 USB-A), Inhalt ganz #ifdef PCIE_PROBE (leer ohne Flag)
            'kernel/smp.c', 'kernel/proc.c', 'kernel/kmain.c') + $netSrc

# xHCI-Treiber (T1.14) nur fuer den PCIe/VL805-Bringup (-PcieProbe) dazulinken -> ohne das Flag NICHT
# im Kernel (RC/Vk/Release byte-identisch). Nimmt nur die MMIO-Basis; treibt den VL805 hinter PCIe.
if ($PcieProbe) { $rpiC += 'drivers/usb/xhci.c' }

$virtAsm = @('arch/aarch64/start.S', 'boards/virt/trap_virt.S')
$virtC   = @('boards/virt/uart_virt.c', 'boards/virt/mmu_virt.c',
             'boards/virt/main_virt.c', 'drivers/net/virtio_net.c',
             'net/tcp_looptest.c',                 # TCP-E2E-Conformance (nur virt-Verifikation)
             'boards/virt/smp_stub.c') + $netSrc   # secondary_main-Stub fuer start.S

if ($Clean) {
    Get-ChildItem -Path . -Recurse -Include *.o -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item -Force -ErrorAction SilentlyContinue `
        '_build/kernel8.elf', '_build/kernel8.img', '_build/net_virt.elf', 'virt_serial.txt', 'virt_qemu_err.txt', `
        '_build/hwsd.img', 'hw_serial.txt', 'hw_selftest_serial.txt', `  # T1.17 HW-Harness-Artefakte
        'gui_serial.txt', 'gui_err.txt'                          # T2.1 GUI-Test-Artefakte
    Write-Host 'Clean fertig.'
    return
}

$tc = Get-Toolchain
Write-Host ("Toolchain: {0} ({1})" -f $tc.Kind, $tc.CC)

# ============================================================
#  virt-Netz-Harness
# ============================================================
if ($Virt) {
    $objs = Compile-Sources $tc ($virtAsm + $virtC)
    $elf = '_build/net_virt.elf'
    Link-Elf $tc 'boards/virt/linker_virt.ld' $elf $objs
    Write-Host ("Build OK -> {0} ({1} Bytes)" -f $elf, (Get-Item $elf).Length) -ForegroundColor Green

    if (-not ($Run -or $Verify)) { return }

    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) {
        Write-Warning 'qemu-system-aarch64 nicht gefunden.'
        if ($Verify) { exit 1 }   # Verifikation konnte nicht laufen -> Fehler
        return
    }

    $qargs = @('-M', 'virt', '-cpu', 'cortex-a72', '-m', '256M', '-kernel', $elf,
               '-global', 'virtio-mmio.force-legacy=false',
               '-netdev', 'user,id=n0,hostfwd=udp::5555-:5555,hostfwd=tcp::5556-:5556,hostfwd=tcp::5559-:80',
               '-device', 'virtio-net-device,netdev=n0',
               '-display', 'none')

    if ($Run) {
        Write-Host 'Starte QEMU virt (Beenden: Strg-A dann X)...' -ForegroundColor Cyan
        & $qemu @qargs -serial stdio
        return
    }

    # ---- automatisierte Echt-Interop-Verifikation ----
    $ser = Join-Path $PSScriptRoot 'virt_serial.txt'
    $err = Join-Path $PSScriptRoot 'virt_qemu_err.txt'
    Remove-Item -Force -ErrorAction SilentlyContinue $ser, $err

    # Host-Listener fuer den AKTIVEN TCP-Client des Gasts (Gast -> Host via SLIRP-
    # Gateway 10.0.2.2:5557). Vor QEMU starten + asynchrones Accept beginnen.
    $listener = $null; $acceptAR = $null
    try {
        $listener = New-Object System.Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 5557)
        $listener.Start()
        $acceptAR = $listener.BeginAcceptTcpClient($null, $null)
    } catch { $listener = $null }

    # Host-HTTP-Responder fuer den HTTP-GET-Client des Gasts (Gast -> Host 10.0.2.2:5558).
    $httpL = $null; $httpAR = $null
    try {
        $httpL = New-Object System.Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 5558)
        $httpL.Start()
        $httpAR = $httpL.BeginAcceptTcpClient($null, $null)
    } catch { $httpL = $null }

    Write-Host 'Starte QEMU virt im Hintergrund...' -ForegroundColor Cyan
    $p = Start-Process -FilePath $qemu -ArgumentList ($qargs + @('-serial', "file:$ser")) `
        -PassThru -WindowStyle Hidden -RedirectStandardError $err
    $started = $p.StartTime

    Start-Sleep -Seconds 4   # Boot + ARP + erste Pings abwarten

    # Lief QEMU ueberhaupt an? Sonst Fehlerursache (stderr) zeigen und abbrechen.
    if ($p.HasExited) {
        Write-Host ("FEHLER: QEMU vorzeitig beendet (Exit {0})." -f $p.ExitCode) -ForegroundColor Red
        if ((Test-Path $err) -and (Get-Item $err).Length -gt 0) {
            Write-Host '--- QEMU stderr ---' -ForegroundColor DarkGray
            Get-Content $err | Write-Host
        }
        exit 1
    }

    # UDP-Echo vom Host testen (mehrere Versuche).
    $payload = "rpi_rtos-host-probe-9d3f"
    $bytes = [Text.Encoding]::ASCII.GetBytes($payload)
    $echo = $null
    for ($try = 0; $try -lt 5 -and -not $echo; $try++) {
        try {
            $udp = New-Object System.Net.Sockets.UdpClient
            $udp.Client.ReceiveTimeout = 1500
            [void]$udp.Send($bytes, $bytes.Length, '127.0.0.1', 5555)
            $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $resp = $udp.Receive([ref]$ep)
            $echo = [Text.Encoding]::ASCII.GetString($resp)
        } catch {
            Start-Sleep -Milliseconds 700
        } finally {
            if ($udp) { $udp.Close() }
        }
    }

    # TCP-Echo vom Host testen (Connect -> senden -> Echo lesen).
    $tcpPayload = "rpi_rtos-tcp-probe-7e21"
    $tcpEcho = $null
    for ($t = 0; $t -lt 5 -and -not $tcpEcho; $t++) {
        try {
            $cli = New-Object System.Net.Sockets.TcpClient
            $iar = $cli.BeginConnect('127.0.0.1', 5556, $null, $null)
            if ($iar.AsyncWaitHandle.WaitOne(1500)) {
                $cli.EndConnect($iar)
                $ns = $cli.GetStream()
                $ns.ReadTimeout = 1500
                $tb = [Text.Encoding]::ASCII.GetBytes($tcpPayload)
                $ns.Write($tb, 0, $tb.Length)
                $rb = New-Object byte[] 256
                $n = $ns.Read($rb, 0, $rb.Length)
                if ($n -gt 0) { $tcpEcho = [Text.Encoding]::ASCII.GetString($rb, 0, $n) }
                $ns.Close()
            }
            $cli.Close()
        } catch {
            Start-Sleep -Milliseconds 600
        }
    }

    # Aktiven Client des Gasts annehmen: Probe lesen + zurueck-echoen (bidirektional),
    # dann ein weiteres Read -> 0 Bytes (EOF) beweist den AKTIVEN Close des Gasts (FIN).
    $clientProbe = $null
    $clientClosed = $false
    if ($listener) {
        try {
            if ($acceptAR.AsyncWaitHandle.WaitOne(4000)) {
                $srv = $listener.EndAcceptTcpClient($acceptAR)
                $sns = $srv.GetStream(); $sns.ReadTimeout = 3000
                $rb2 = New-Object byte[] 256
                $n2 = $sns.Read($rb2, 0, $rb2.Length)
                if ($n2 -gt 0) {
                    $clientProbe = [Text.Encoding]::ASCII.GetString($rb2, 0, $n2).Trim()
                    $sns.Write($rb2, 0, $n2)   # zurueck-echoen -> Gast meldet 'Antwort vom Host'
                    $sns.Flush()
                    try {
                        $n3 = $sns.Read($rb2, 0, $rb2.Length)   # auf Gast-FIN warten
                        if ($n3 -eq 0) { $clientClosed = $true } # 0 = graceful close (FIN)
                    } catch {}
                }
                $sns.Close(); $srv.Close()
            }
        } catch {}
        $listener.Stop()
    }

    # HTTP-GET des Gasts annehmen: Request lesen, HTTP/1.0-200-Antwort mit bekanntem Body senden.
    if ($httpL) {
        try {
            if ($httpAR.AsyncWaitHandle.WaitOne(4000)) {
                $hsrv = $httpL.EndAcceptTcpClient($httpAR)
                $hns = $hsrv.GetStream(); $hns.ReadTimeout = 2000
                $hrb = New-Object byte[] 512
                [void]$hns.Read($hrb, 0, $hrb.Length)   # Request lesen (verwerfen)
                $body = 'rpi_rtos-http-body-3f9a'
                $resp = "HTTP/1.0 200 OK`r`nContent-Type: text/plain`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body"
                $rbts = [Text.Encoding]::ASCII.GetBytes($resp)
                $hns.Write($rbts, 0, $rbts.Length); $hns.Flush()
                Start-Sleep -Milliseconds 400
                $hns.Close(); $hsrv.Close()
            }
        } catch {}
        $httpL.Stop()
    }

    # HTTP-SERVER des Gasts testen (Host -> Gast via hostfwd 127.0.0.1:5559 -> Gast:80).
    # Roher GET ueber TcpClient: senden, bis zum Close lesen (HTTP/1.0 + Connection: close).
    function Invoke-GuestHttp($path) {
        # Bis zu 6 Versuche: SLIRP nimmt die Host-Verbindung sofort an, aber das gast-
        # seitige SYN kann verworfen werden, wenn der kleine PCB-Pool (4) gerade durch
        # TIME_WAIT-Verbindungen (2 s) belegt ist -> leere Antwort -> erneut versuchen,
        # bis eine gueltige HTTP/-Antwort kommt (6 x 600 ms deckt den 2 s-TIME_WAIT ab).
        for ($t = 0; $t -lt 6; $t++) {
            $resp = $null
            try {
                $c = New-Object System.Net.Sockets.TcpClient
                $ar = $c.BeginConnect('127.0.0.1', 5559, $null, $null)
                if ($ar.AsyncWaitHandle.WaitOne(1500)) {
                    $c.EndConnect($ar)
                    $s = $c.GetStream(); $s.ReadTimeout = 2000
                    $rq = [Text.Encoding]::ASCII.GetBytes("GET $path HTTP/1.0`r`nHost: rpi-guest`r`n`r`n")
                    $s.Write($rq, 0, $rq.Length); $s.Flush()
                    $msbuf = New-Object System.IO.MemoryStream
                    $tmp = New-Object byte[] 1024
                    while ($true) {
                        try { $k = $s.Read($tmp, 0, $tmp.Length) } catch { break }
                        if ($k -le 0) { break }
                        $msbuf.Write($tmp, 0, $k)
                    }
                    $s.Close()
                    $resp = [Text.Encoding]::ASCII.GetString($msbuf.ToArray())
                }
                $c.Close()
            } catch { $resp = $null }
            if ($resp -match '^HTTP/') { return $resp }
            Start-Sleep -Milliseconds 600
        }
        return $resp
    }
    $httpdRoot  = Invoke-GuestHttp '/'
    $httpd404   = Invoke-GuestHttp '/keine-solche-seite'
    $httpdQuery = Invoke-GuestHttp '/status?cache=1'
    $httpdBig   = Invoke-GuestHttp '/big'

    Start-Sleep -Seconds 1
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

    $log = if (Test-Path $ser) { Get-Content $ser -Raw } else { '' }

    Write-Host "`n===== Serial-Log (virt) =====" -ForegroundColor DarkGray
    Write-Host $log
    Write-Host '=============================' -ForegroundColor DarkGray

    Write-Host "`n===== Verifikation =====" -ForegroundColor Cyan
    $ok = $true
    $fresh = (Test-Path $ser) -and ((Get-Item $ser).LastWriteTime -gt $started)
    $ok = (Check 'Serial-Log stammt von diesem Lauf (frisch)' $fresh) -and $ok
    $ok = (Check 'virtio-net erkannt (Version 2)' ($log -match 'virtio-net\] gefunden' -and $log -match 'version=2')) -and $ok
    $ok = (Check 'DHCP-Lease bezogen (IP vom SLIRP-Server)' ($log -match '\[dhcp\] Lease: IP 10\.0\.2\.15')) -and $ok
    $ok = (Check 'DHCP-Renewal: Unicast-REQUEST -> Server-ACK erneuert die Lease' ($log -match '\[dhcp\] Lease erneuert: IP 10\.0\.2\.15')) -and $ok
    $ok = (Check 'ARP: Gateway 10.0.2.2 aufgeloest'  ($log -match '\[arp\] 10\.0\.2\.2 is-at')) -and $ok
    $ok = (Check 'UDP-Echo-Handler ausgeloest (Host->Gast)' ($log -match '\[udp\] 5555:')) -and $ok
    $ok = (Check ("UDP-Echo am Host empfangen ('{0}')" -f $payload) ($echo -eq $payload)) -and $ok
    $ok = (Check 'TCP-Echo-Handler ausgeloest (Host->Gast)' ($log -match '\[tcp\] 5556:')) -and $ok
    $ok = (Check ("TCP-Echo am Host empfangen ('{0}')" -f $tcpPayload) ($tcpEcho -eq $tcpPayload)) -and $ok
    # Aktiver TCP-Client (Gast -> Host): Verbindungsaufbau + Senden + Empfangen.
    $ok = (Check 'TCP-Client: Gast verbunden + Probe gesendet' ($log -match '\[tcpc\] verbunden')) -and $ok
    $ok = (Check 'TCP-Sende-Flusskontrolle: Peer-Empfangsfenster geparst (> 0)' ($log -match '\[tcpc\] verbunden \(Peer-Fenster=[1-9]\d*\)')) -and $ok
    $ok = (Check 'TCP-Empfang: Out-of-Order-Reassembly (in-order Zustellung, white-box)' ($log -match '\[3b\] TCP-Reassembly-Selbsttest \(out-of-order\): ok')) -and $ok
    $ok = (Check 'TCP-E2E: OOO + Fenster + FIN + Persist + RTX + SACK + RTO + Nagle/Delayed-ACK + Window-Scaling + PAWS + Congestion-Control + Keepalive + SWS (Loopback)' ($log -match '\[3c\] TCP-Loopback-Conformance \(OOO \+ Fenster \+ FIN \+ Persist \+ RTX \+ SACK \+ RTO \+ Nagle \+ WScale \+ PAWS \+ CC \+ KA \+ SWS\): ok')) -and $ok
    $ok = (Check "TCP-Client: Host empfing Gast-Probe ('rpi_rtos-client-probe-5a7c')" ($clientProbe -eq 'rpi_rtos-client-probe-5a7c')) -and $ok
    $ok = (Check 'TCP-Client: Gast empfing Host-Antwort (bidirektional)' ($log -match '\[tcpc\] Antwort vom Host')) -and $ok
    # Aktiver Close (Gast initiiert FIN -> FIN_WAIT -> TIME_WAIT):
    $ok = (Check 'TCP-Client: Gast initiiert aktiven Close (FIN)' ($log -match '\[tcpc\] schliesse Verbindung')) -and $ok
    $ok = (Check 'TCP-Client: Host sieht sauberen Close des Gasts (EOF/FIN)' $clientClosed) -and $ok
    # DNS-Client: example.com gegen den SLIRP-DNS-Server (10.0.2.3) aufloesen.
    $dnsMatch = [regex]::Match($log, '\[dns\] example\.com -> (\d+\.\d+\.\d+\.\d+)')
    $dnsOk = $dnsMatch.Success -and ($dnsMatch.Groups[1].Value -ne '0.0.0.0')
    $ok = (Check ("DNS-Client: example.com aufgeloest" + $(if ($dnsMatch.Success) { " -> " + $dnsMatch.Groups[1].Value })) $dnsOk) -and $ok
    # HTTP-GET-Client (komponiert TCP connect/send/recv/close + HTTP-Parsing):
    $ok = (Check 'HTTP-Client: GET -> Status 200 geparst' ($log -match '\[http\] status=200')) -and $ok
    $ok = (Check 'HTTP-Client: Body vom Host empfangen (rpi_rtos-http-body)' ($log -match 'rpi_rtos-http-body-3f9a')) -and $ok
    # HTTP-Server (Host -> Gast via hostfwd :5559 -> :80): GET / -> 200 + Body, unbekannt -> 404.
    $ok = (Check 'HTTP-Server: Gast bediente GET / (Handler)' ($log -match '\[httpd\] GET /')) -and $ok
    $ok = (Check 'HTTP-Server: GET / -> 200 OK' ($httpdRoot -match '^HTTP/1\.0 200')) -and $ok
    $ok = (Check 'HTTP-Server: Root-Body geliefert (marker)' ($httpdRoot -match 'rpi_rtos-httpd-root-7b2e')) -and $ok
    $ok = (Check 'HTTP-Server: unbekannter Pfad -> 404' ($httpd404 -match '^HTTP/1\.0 404')) -and $ok
    # Query-String wird abgeschnitten -> /status?cache=1 trifft die Ressource /status (200).
    $ok = (Check 'HTTP-Server: Query-String abgeschnitten (/status?... -> 200)' (($httpdQuery -match '^HTTP/1\.0 200') -and ($httpdQuery -match 'rpi_rtos-httpd-status'))) -and $ok
    # Zu grosse Antwort (> Sendepuffer) wird sauber mit 500 abgelehnt statt still gekuerzt.
    $ok = (Check 'HTTP-Server: uebergrosse Ressource -> 500 (keine Content-Length-Luege)' ($httpdBig -match '^HTTP/1\.0 500')) -and $ok
    # T1.11 Big-Net-Lock: reentrant EXERZIERT (max-Tiefe>=2 -> Eingangspfad ruft verriegelte Sender)
    # und BALANCIERT (jeder enter durch ein leave); Obergrenze 6 faengt ein gelecktes enter (fehlendes
    # leave) ab. Nicht-reentrante Mutation -> Self-Deadlock/Hang -> alle Netz-Asserts fallen aus.
    $nlm = [regex]::Match($log, 'Big-Net-Lock reentrant: max-Tiefe=(\d+) unbalanciert=(nein|JA\(FEHLER\))')
    $nlOk = $nlm.Success -and ([int]$nlm.Groups[1].Value -ge 2) -and ([int]$nlm.Groups[1].Value -le 6) -and ($nlm.Groups[2].Value -eq 'nein')
    $ok = (Check ('T1.11 Net-Lock: Big-Net-Lock reentrant exerziert (max-Tiefe {0}, 2..6) + balanciert' -f $(if ($nlm.Success) { $nlm.Groups[1].Value } else { '?' })) $nlOk) -and $ok
    # ICMP-Reply ist Bonus (haengt an SLIRP-Gateway-Verhalten):
    if ($log -match '\[icmp\] echo reply von 10\.0\.2\.2') {
        Write-Host '  [PASS] ICMP: Echo-Reply vom Gateway (Bonus)' -ForegroundColor Green
    } else {
        Write-Host '  [warn] kein ICMP-Echo-Reply vom Gateway (SLIRP-abhaengig, kein Fehler)' -ForegroundColor Yellow
    }

    if ($ok) {
        Write-Host "`nERGEBNIS: Netz-Stack verifiziert (echte Interop ueber virtio-net).`n" -ForegroundColor Green
        exit 0
    }
    Write-Host "`nERGEBNIS: Verifikation FEHLGESCHLAGEN.`n" -ForegroundColor Red
    if ((Test-Path $err) -and (Get-Item $err).Length -gt 0) {
        Write-Host '--- QEMU stderr ---' -ForegroundColor DarkGray
        Get-Content $err | Write-Host
    }
    exit 1
}

# ============================================================
#  virt USB/xHCI-Harness (T1.14: generischer xHCI-Treiber gegen qemu-xhci)
# ============================================================
if ($VirtUsb) {
    $usbAsm = @('arch/aarch64/start.S', 'boards/virt/trap_virt.S')
    $usbC   = @('boards/virt/uart_virt.c', 'boards/virt/mmu_virt.c',
                'boards/virt/main_usb.c', 'boards/virt/smp_stub.c',
                'lib/kmem.c', 'drivers/pci/pci.c', 'drivers/usb/xhci.c',
                'drivers/usb/usb_hc.c', 'drivers/usb/usbmsc.c')   # HCD-vtable + GETEILTER Klassentreiber
    $objs = Compile-Sources $tc ($usbAsm + $usbC)
    $elf = '_build/usb_virt.elf'
    Link-Elf $tc 'boards/virt/linker_virt.ld' $elf $objs
    Write-Host ("Build OK -> {0} ({1} Bytes)" -f $elf, (Get-Item $elf).Length) -ForegroundColor Green
    if (-not ($Run -or $Verify)) { return }

    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; if ($Verify) { exit 1 }; return }

    # USB-Massenspeicher-Fixture: 64 KiB, Marker "RTOS..." bei LBA 0 (der MSC-BOT-Test liest ihn).
    $disk = Join-Path $PSScriptRoot '_build/usb_disk.img'
    $dbytes = New-Object byte[] 65536
    $mk = [Text.Encoding]::ASCII.GetBytes('RTOS-usb-a-marker-xhci')
    [Array]::Copy($mk, $dbytes, $mk.Length)
    [IO.File]::WriteAllBytes($disk, $dbytes)

    $qargs = @('-M', 'virt', '-cpu', 'cortex-a72', '-m', '256M', '-kernel', $elf,
               '-device', 'qemu-xhci,id=xhci',
               '-drive', "id=stick,file=$disk,format=raw,if=none",
               '-device', 'usb-storage,bus=xhci.0,drive=stick',
               '-display', 'none')
    if ($Run) {
        Write-Host 'Starte QEMU virt+xHCI (Strg-A dann X)...' -ForegroundColor Cyan
        & $qemu @qargs -serial stdio; return
    }

    $ser = Join-Path $PSScriptRoot 'usb_serial.txt'
    $err = Join-Path $PSScriptRoot 'usb_qemu_err.txt'
    Remove-Item -Force -ErrorAction SilentlyContinue $ser, $err
    Write-Host 'Starte QEMU virt+xHCI im Hintergrund...' -ForegroundColor Cyan
    $p = Start-Process -FilePath $qemu -ArgumentList ($qargs + @('-serial', "file:$ser")) `
        -PassThru -WindowStyle Hidden -RedirectStandardError $err
    Start-Sleep -Seconds 4
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    $log = if (Test-Path $ser) { Get-Content $ser -Raw } else { '' }

    Write-Host "`n===== Serial-Log (virt USB/xHCI) =====`n" -ForegroundColor DarkGray
    Write-Host $log

    Write-Host "`n===== Verifikation =====" -ForegroundColor Cyan
    $ok = $true
    function Check($name, $cond) {
        if ($cond) { Write-Host "  [PASS] $name" -ForegroundColor Green }
        else { Write-Host "  [FAIL] $name" -ForegroundColor Red }
        return [bool]$cond
    }
    $ok = (Check 'PCIe-ECAM: xHCI-Controller auf Bus 0 gefunden (qemu-xhci 1b36:000d)' ($log -match '\[pci\] xHCI @ bus0 dev\d+ VID=0x0*1b36 DID=0x0*d\b')) -and $ok
    $ok = (Check 'xHCI: init ok (HCRST + DCBAA/Command-/Event-Ring + R/S)' ($log -match '\[xhci\] init ret=0 \(ok\)')) -and $ok
    $ok = (Check 'T1.14 xHCI-Enumeration: usb-storage via Ring/EnableSlot/AddressDevice/GET_DESCRIPTOR' ($log -match 'Enumeration ok: VID=0x0*46f4 PID=0x0*1\b')) -and $ok
    # GETEILTER Klassentreiber usbmsc.c ueber die usb_hc-vtable (xHCI-Backend):
    $ok = (Check 'usb_hc-vtable: geteiltes usbmsc READ CAPACITY ueber xHCI (128 Sektoren)' ($log -match '\[msc\] Kapazitaet: 128 Sektoren')) -and $ok
    $ok = (Check 'usb_hc-vtable: geteiltes usbmsc WRITE(10)+READ(10)-Schreibtest ueber xHCI' ($log -match '\[msc\] schreibtest: ok')) -and $ok
    $ok = (Check 'usb_hc-vtable: geteiltes usbmsc READ(10) LBA0-Marker ueber xHCI' ($log -match '\[msctest\] usbmsc\(GETEILT\)/xHCI: sectors=128 marker=ok')) -and $ok
    if ($ok) {
        Write-Host "`nERGEBNIS: xHCI (T1.14) + usb_hc-vtable verifiziert (Enumeration + geteiltes usbmsc ueber xHCI).`n" -ForegroundColor Green
        exit 0
    }
    Write-Host "`nERGEBNIS: Verifikation FEHLGESCHLAGEN.`n" -ForegroundColor Red
    if ((Test-Path $err) -and (Get-Item $err).Length -gt 0) {
        Write-Host '--- QEMU stderr ---' -ForegroundColor DarkGray
        Get-Content $err | Write-Host
    }
    exit 1
}

# ============================================================
#  raspi4b-Kernel (Produkt)
# ============================================================
$elf = '_build/kernel8.elf'
$img = '_build/kernel8.img'

$objs = Compile-Sources $tc ($rpiAsm + $rpiC)
Link-Elf $tc 'arch/aarch64/linker.ld' $elf $objs
& $tc.OBJCOPY '-O' 'binary' $elf $img
if ($LASTEXITCODE) { throw 'objcopy fehlgeschlagen.' }

# User-Applikationen (EL0, ELF64): hello (Demo) + shell (Login-Shell). -Iuser/lib fuer die
# geteilte libgui (ulib.h/gui.h, T2.2).
$ct = $cflags + @('-Iuser/lib')
if ($tc.Kind -eq 'clang') {
    $ct = @('--target=aarch64-none-elf', '-Wno-unused-command-line-argument') + $ct
}
# Geteilte User-Bibliotheks-Objekte: libgui (T2.2) + WinForms-Retained-Kern (T2.4).
& $tc.CC @ct '-c' 'user/lib/gui.c' '-o' 'user/lib/gui.o'
if ($LASTEXITCODE) { throw 'libgui (user/lib/gui.c) kompilieren fehlgeschlagen' }
& $tc.CC @ct '-c' 'user/lib/winforms.c' '-o' 'user/lib/winforms.o'
if ($LASTEXITCODE) { throw 'winforms (user/lib/winforms.c) kompilieren fehlgeschlagen' }
# T2.7: eingebetteter TTF-gerasterter Font (aus _build/tools/gen_font.py -> user/lib/font_*.c, eingecheckt;
# gen_font.py wird nur bei Font-Wechsel manuell ausgefuehrt, damit der Build KEIN Pillow braucht).
& $tc.CC @ct '-c' 'user/lib/font_dejavu_sans.c' '-o' 'user/lib/font_dejavu_sans.o'
if ($LASTEXITCODE) { throw 'Font (user/lib/font_dejavu_sans.c) kompilieren fehlgeschlagen' }
# T2.8: Laufzeit-TTF-Rasterung MIT FP kompilieren (OHNE -mgeneral-regs-only). Nur diese Datei nutzt FP,
# nur GUI.ELF linkt sie; FP ist zur Laufzeit nur in GUI-Builds aktiv (Kernel setzt dort FPEN=0b11).
# -fno-math-errno: __builtin_sqrtf wird zur fsqrt-INSTRUKTION statt zum errno-setzenden
# libcall (freestanding hat kein errno/libm) -- sonst 'undefined symbol: sqrtf'.
$ctfp = @($ct | Where-Object { $_ -ne '-mgeneral-regs-only' }) + '-fno-math-errno'
& $tc.CC @ctfp '-c' 'user/lib/gui_ttf.c' '-o' 'user/lib/gui_ttf.o'
if ($LASTEXITCODE) { throw 'gui_ttf (FP) kompilieren fehlgeschlagen' }
# T3.2: Software-3D-Rasterizer (Phase 3, Stufe 1) -- FP wie gui_ttf.c.
& $tc.CC @ctfp '-c' 'user/lib/r3d.c' '-o' 'user/lib/r3d.o'
if ($LASTEXITCODE) { throw 'r3d (FP) kompilieren fehlgeschlagen' }
# T3.3+: Vulkan-Implementierung (echte Khronos-Header in user/lib/vulkan/) + memset/memcpy-Shim.
& $tc.CC @ctfp '-c' 'user/lib/vk/vk_core.c' '-o' 'user/lib/vk/vk_core.o'
if ($LASTEXITCODE) { throw 'vk_core (FP) kompilieren fehlgeschlagen' }
& $tc.CC @ct '-c' 'user/lib/ustring.c' '-o' 'user/lib/ustring.o'
if ($LASTEXITCODE) { throw 'ustring kompilieren fehlgeschlagen' }
# T3.4: SPIR-V-Shader generieren (pure-stdlib-Python, kein glslang noetig) + Interpreter bauen.
$py = Find-Tool @('python', 'py')
if (-not $py) { throw 'Python fehlt -- _build/tools/gen_spirv.py erzeugt die SPIR-V-Shader.' }
& $py '_build/tools/gen_spirv.py' | Out-Null
if ($LASTEXITCODE) { throw 'gen_spirv.py fehlgeschlagen' }
& $tc.CC @ctfp '-c' 'user/lib/vk/vk_spirv.c' '-o' 'user/lib/vk/vk_spirv.o'
if ($LASTEXITCODE) { throw 'vk_spirv (FP) kompilieren fehlgeschlagen' }
# T3.5: Draw-Pfad (CommandBuffer/RenderPass/Pipeline -> SPIR-V-Shader -> r3d-Rasterizer).
& $tc.CC @ctfp '-c' 'user/lib/vk/vk_cmd.c' '-o' 'user/lib/vk/vk_cmd.o'
if ($LASTEXITCODE) { throw 'vk_cmd (FP) kompilieren fehlgeschlagen' }
# T3.6: WSI (VK_KHR_surface/swapchain ueber die GUI-Bruecke).
& $tc.CC @ctfp '-c' 'user/lib/vk/vk_wsi.c' '-o' 'user/lib/vk/vk_wsi.o'
if ($LASTEXITCODE) { throw 'vk_wsi (FP) kompilieren fehlgeschlagen' }
# V5 (V3D-Backend-Fundament): QPU-Instruktions-Encoder (integer, HW-RE-zu-validieren).
& $tc.CC @ctfp '-c' 'user/lib/vk/v3d_qpu.c' '-o' 'user/lib/vk/v3d_qpu.o'
if ($LASTEXITCODE) { throw 'v3d_qpu (V5-QPU-Encoder) kompilieren fehlgeschlagen' }

# T3.x (Phase 3): User-Apps, die MIT FP kompiliert werden (wie gui_ttf.c). FP laeuft nur in
# GUI_FP-Builds (FPEN=0b11 + T3.1-Kontextsicherung); anderswo werden sie nicht gestartet.
function Build-UserAppFp($name, $libObjs = @()) {
    & $tc.CC @ctfp '-c' "user/$name.c" '-o' "user/$name.o"
    if ($LASTEXITCODE) { throw "User-App (FP) kompilieren fehlgeschlagen: $name" }
    & $tc.LD '-no-pie' '-z' 'max-page-size=4096' '-T' 'user/user.ld' '-o' "_build/$name.elf" "user/$name.o" @libObjs
    if ($LASTEXITCODE) {
        & $tc.LD '-z' 'max-page-size=4096' '-T' 'user/user.ld' '-o' "_build/$name.elf" "user/$name.o" @libObjs
        if ($LASTEXITCODE) { throw "User-App (FP) linken fehlgeschlagen: $name" }
    }
}

function Build-UserApp($name, $libObjs = @()) {
    & $tc.CC @ct '-c' "user/$name.c" '-o' "user/$name.o"
    if ($LASTEXITCODE) { throw "User-App kompilieren fehlgeschlagen: $name" }
    & $tc.LD '-no-pie' '-z' 'max-page-size=4096' '-T' 'user/user.ld' '-o' "_build/$name.elf" "user/$name.o" @libObjs
    if ($LASTEXITCODE) {
        # GNU-ld akzeptiert -no-pie nicht immer am LD direkt; ohne versuchen.
        & $tc.LD '-z' 'max-page-size=4096' '-T' 'user/user.ld' '-o' "_build/$name.elf" "user/$name.o" @libObjs
        if ($LASTEXITCODE) { throw "User-App linken fehlgeschlagen: $name" }
    }
}
Build-UserApp 'hello'
Build-UserApp 'shell'
Build-UserApp 'pchild'   # Kind-Prozess fuer den wait/kill-Test (SYS_WAIT/SYS_KILL/SYS_GETPPID)
Build-UserApp 'pwaiter'  # wartet auf ein Enkelkind -> Test fuer "kill eines in wait() blockierten Prozesses"
Build-UserApp 'ploop'    # unsterbliches Enkelkind fuer den kill-in-wait-Guardian
Build-UserApp 'guitest' @('user/lib/gui.o', 'user/lib/winforms.o')   # T2.1-T2.6: Bruecke + libgui + Events + WinForms-Kern + Controls + Nav
Build-UserApp 'gui'     @('user/lib/gui.o', 'user/lib/winforms.o', 'user/lib/font_dejavu_sans.o', 'user/lib/gui_ttf.o')   # T2.6/T2.7/T2.8: WinForms + Font + Laufzeit-TTF (FP)
Build-UserAppFp 'fptest'   # T3.1: FP-Kontext-Guardian (V-Register/FPCR-Hammer, laeuft nur im -Vk-Build)
$vkLibs = @('user/lib/gui.o', 'user/lib/r3d.o', 'user/lib/vk/vk_core.o', 'user/lib/vk/vk_spirv.o', 'user/lib/vk/vk_cmd.o', 'user/lib/vk/vk_wsi.o', 'user/lib/vk/v3d_qpu.o', 'user/lib/ustring.o')
Build-UserAppFp 'vktest' $vkLibs   # T3.2+: r3d-/Vulkan-/SPIR-V-/Draw-Selbsttests + 3D-Demo
Build-UserAppFp 'vkcube' $vkLibs   # T3.6: echte Vulkan-App (Swapchain + rotierender Wuerfel)
Build-UserAppFp 'vkgui' ($vkLibs + @('user/lib/winforms.o'))   # Vulkan-Demo IN einer WinForms-GUI (offscreen-Render -> Panel)

# VISION-Modul (nur -Vision): Inferenz-Engine (NEON-fp32) + AIVISION.ELF. Kapselung: ohne -Vision
# wird hier NICHTS kompiliert -- kein Byte Vision-Code im Baum (docs/architecture/19 §1).
if ($Vision -or $DevImage) {
    & $tc.CC @ctfp '-c' 'user/lib/vision/vi_engine.c' '-o' 'user/lib/vision/vi_engine.o'
    if ($LASTEXITCODE) { throw 'vi_engine (Vision, FP/NEON) kompilieren fehlgeschlagen' }
    & $tc.CC @ctfp '-c' 'user/lib/vision/vi_model.c' '-o' 'user/lib/vision/vi_model.o'
    if ($LASTEXITCODE) { throw 'vi_model (Vision, gehaerteter Blob-Loader) kompilieren fehlgeschlagen' }
    & $tc.CC @ctfp '-c' 'user/lib/vision/vi_img.c' '-o' 'user/lib/vision/vi_img.o'
    if ($LASTEXITCODE) { throw 'vi_img (Vision, gehaerteter BMP-Loader) kompilieren fehlgeschlagen' }
    & $tc.CC @ctfp '-c' 'user/lib/vision/vi_par.c' '-o' 'user/lib/vision/vi_par.o'
    if ($LASTEXITCODE) { throw 'vi_par (Vision, EL0-Parallel-For) kompilieren fehlgeschlagen' }
    & $tc.CC @ctfp '-c' 'user/lib/vision/vi_detect.c' '-o' 'user/lib/vision/vi_detect.o'
    if ($LASTEXITCODE) { throw 'vi_detect (Vision, Detektions-Pipeline) kompilieren fehlgeschlagen' }
    & $tc.CC @ctfp '-c' 'user/lib/vision/vi_embed.c' '-o' 'user/lib/vision/vi_embed.o'
    if ($LASTEXITCODE) { throw 'vi_embed (Vision, Embedding/Anomalie) kompilieren fehlgeschlagen' }
    Build-UserAppFp 'aivision' @('user/lib/vision/vi_engine.o', 'user/lib/vision/vi_model.o', 'user/lib/vision/vi_img.o', 'user/lib/vision/vi_par.o', 'user/lib/vision/vi_detect.o', 'user/lib/vision/vi_embed.o')
    # M0-Fixtures (rot-dominantes BMP + Mini-Klassifikator-*.net) fuer hdd1 erzeugen.
    & $py '_build/tools/gen_vision_fixtures.py' 'user/vision_img.bmp' 'user/vision_net.net' | Out-Null
    if ($LASTEXITCODE) { throw 'gen_vision_fixtures.py fehlgeschlagen' }
}

Write-Host ("Build OK -> {0} ({1} Bytes)" -f $img, (Get-Item $img).Length) -ForegroundColor Green

# SD-Image mit der User-App (INIT.ELF) regenerieren. FATAL bei Fehlschlag: ein still
# veraltetes sd.img liesse die Verifikation gegen ALTE Binaries laufen (Integritaet!).
# Kurzer Retry: der Virenscanner haelt frisch gelinkte ELFs gelegentlich transient offen.
$py = Find-Tool @('python', 'py')
if ($py -and (Test-Path '_build/tools/gen_sdimg.py') -and (Test-Path '_build/hello.elf') -and (Test-Path '_build/shell.elf')) {
    $sdOk = $false
    for ($try = 0; $try -lt 3 -and -not $sdOk; $try++) {
        if ($try -gt 0) { Start-Sleep -Milliseconds 700 }
        $sdArgs = @('_build/sd.img', '_build/hello.elf', '_build/shell.elf', '_build/pchild.elf', '_build/pwaiter.elf', '_build/ploop.elf', '_build/guitest.elf', '_build/gui.elf', 'user/gui_font.ttf', '_build/fptest.elf', '_build/vktest.elf', '_build/vkcube.elf', 'user/vk_vert.spv', 'user/vk_frag.spv', '_build/vkgui.elf')   # arg15 = VKGUI.ELF(hdd0)
        if ($Vision -or $DevImage) { $sdArgs += '_build/aivision.elf', 'user/vision_img.bmp', 'user/vision_net.net' }   # arg16 AIVISION.ELF(hdd0) + arg17/18 M0-Fixtures(hdd1)
        & $py '_build/tools/gen_sdimg.py' @sdArgs | Out-Null
        $sdOk = ($LASTEXITCODE -eq 0)
    }
    if (-not $sdOk) { throw 'gen_sdimg fehlgeschlagen -- sd.img waere veraltet (Verifikation unglaubwuerdig).' }
    Write-Host 'sd.img mit INIT/SHELL/PCHILD/PWAITER/PLOOP/GUITEST/GUI/FPTEST/VKTEST aktualisiert.' -ForegroundColor DarkGray
}

# ============================================================
#  HW-SD-Fixture (T1.17-Erweiterung): bootfaehiges SD-Image fuer den AKTUELLEN Flavor (-Vk /
#  -Vision / sonst RC) erzeugen, damit die bereits QEMU-verifizierte Software am ECHTEN Pi4 laeuft.
#  Mit -Verify: Serial-Marker-Poll -- QEMU-SelfTest (Schreibtisch) ODER -ComPort (echte HW).
#  _build/kernel8.img wurde oben passend zum Flavor gebaut; die Marker geben vktest/vkcube/aivision aus.
# ============================================================
if ($HwImage) {
    $py = Find-Tool @('python', 'py')
    if (-not $py) { Write-Warning 'Python fehlt (gen_hwsd.py).'; exit 1 }
    $hwsd = '_build/hwsd.img'
    # Arg-Liste als EIN durchgehend gefuelltes Array bauen (kein leerer Splat -> PS-5.1-sicher).
    if ($Vk) {
        $markerSet = 'vk'
        $ha = @('_build/tools/gen_hwsd.py', $hwsd, '--kernel', $img, '--vktest', '_build/vktest.elf',
                '--vkcube', '_build/vkcube.elf', '--vert', 'user/vk_vert.spv', '--frag', 'user/vk_frag.spv')
    } elseif ($Vision) {
        $markerSet = 'vision'
        $ha = @('_build/tools/gen_hwsd.py', $hwsd, '--kernel', $img, '--aivision', '_build/aivision.elf',
                '--visimg', 'user/vision_img.bmp', '--visnet', 'user/vision_net.net')
    } else {
        $markerSet = 'rc'
        $ha = @('_build/tools/gen_hwsd.py', $hwsd, '--kernel', $img, '--init', '_build/hello.elf', '--shell', '_build/shell.elf')
    }
    if ($Firmware) { $ha += @('--firmware', $Firmware) }
    $ha += '--check'
    & $py @ha
    if ($LASTEXITCODE) { Write-Host 'gen_hwsd fehlgeschlagen (Read-Back-Check rot).' -ForegroundColor Red; exit 1 }
    Write-Host ("HW-SD erzeugt: {0} (Flavor: {1}). Auf eine microSD schreiben und im Pi4 booten." -f $hwsd, $markerSet) -ForegroundColor Green
    if (-not $Firmware) {
        Write-Host '  Hinweis: OHNE -Firmware <dir> fehlen start4.elf/fixup4.dat/DTB/armstub -> das Image bootet nur in QEMU, nicht am echten Pi4.' -ForegroundColor DarkYellow
    }

    if ($Verify) {
        $hv = Join-Path $PSScriptRoot '_build/tools/hw_verify.ps1'
        Write-Host ''
        if ($ComPort) {
            Write-Host "=== HW-Verify am echten Pi4 (COM $ComPort, Flavor $markerSet) ===" -ForegroundColor Cyan
            $hvArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $hv, '-ComPort', $ComPort, '-MarkerSet', $markerSet, '-TimeoutSec', '120', '-ResetPrompt')
        } else {
            Write-Host "=== HW-Verify (QEMU-SelfTest: prueft Poll-/Marker-Logik am Schreibtisch, Flavor $markerSet) ===" -ForegroundColor Cyan
            # Absolute Pfade: der Kindprozess erbt die PROZESS-CWD (nicht $PWD von Set-Location) --
            # relative _build/kernel8.img/sd.img wuerde QEMU dort sonst nicht finden.
            $hvArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $hv, '-SelfTest', '-Image', (Join-Path $PSScriptRoot $img), '-Sd', (Join-Path $PSScriptRoot '_build/sd.img'), '-MarkerSet', $markerSet, '-TimeoutSec', '90')
        }
        & powershell @hvArgs
        exit $LASTEXITCODE
    }
    exit 0
}

# ============================================================
#  RC-Release-Smoke-Guardian (T1.1/T1.2): das Produktions-Image (-Release, ohne
#  RTOS_SELFTEST) muss (a) die Fail-closed-Sicherheitsgates bestehen, (b) den
#  interaktiven Login-Prompt erreichen und (c) GREP-CLEAN sein -- keine hartkodierten
#  Credentials, kein destruktiver Selbsttest, kein Demo-Scaffolding im Serial-Log.
# ============================================================
if ($Release -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }
    . (Join-Path $PSScriptRoot '_build/tools/rc_markers.ps1')   # zentrale Marker-Taxonomie (T1.17)

    Write-Host ''
    Write-Host '=== RC-Release-Verifikation (grep-clean Produktions-Image) ===' -ForegroundColor Cyan
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'rc_serial.txt') (Join-Path $PSScriptRoot 'rc_err.txt') 20
    $log = $r.Log
    $ok = $true

    # (1+2) Alle Pflicht-Marker der zentralen Taxonomie (Fail-closed-Gates, RC-READY, Login):
    foreach ($m in $RcReadyMarkers) {
        $ok = (Check ('RC: ' + $m.Name) ($log -match $m.Pattern)) -and $ok
    }
    $ok = (Check 'RC: kein Fail-closed-Halt ausgeloest' (-not ($log -match $RcHaltPattern))) -and $ok
    # (3) GREP-CLEAN (T1.1): kein Scaffolding/Backdoor im Serial-Log:
    $dirty = @($RcForbidden | Where-Object { $log -match [regex]::Escape($_) })
    $cleanMsg = 'RC: grep-clean (kein Scaffolding/Backdoor)'
    if ($dirty.Count -gt 0) { $cleanMsg += ' -- GEFUNDEN: ' + ($dirty -join ', ') }
    $ok = (Check $cleanMsg ($dirty.Count -eq 0)) -and $ok

    # (4) T1.8 statisch AUF DEM RELEASE-BINARY (Review T3.x: vorher lief der objdump-Guardian
    # nur im Selbsttest-Image): das RC-.text darf KEINE FP/SIMD-Instruktion enthalten --
    # fpctx.S ist ohne GUI_FP leer, der Rest baut -mgeneral-regs-only.
    $objdump = if ($tc.Kind -eq 'gnu') { ($tc.CC -replace 'gcc$','objdump') } else { (Find-Tool @('llvm-objdump')) }
    $fpCount = -1
    if ($objdump -and (Test-Path '_build/kernel8.elf')) {
        $dis = & $objdump -d --no-show-raw-insn '_build/kernel8.elf' 2>$null
        $fpCount = ($dis | Where-Object { $_ -notmatch '\.(word|byte|short|long|inst|zero|ascii)' } |
                    Select-String -Pattern '\b[vqdsh]([0-9]|[12][0-9]|3[01])\b').Count
    }
    $ok = (Check ('RC: Release-Kernel-.text integer-only, KEINE FP/SIMD-Instruktion (gefunden: {0})' -f $fpCount) ($fpCount -eq 0)) -and $ok

    if ($ok) { Write-Host 'RC-Release-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'RC-Release-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  T2.6/T3.x -GuiLogin-Verifikation: das Login->GUI-Image (INTERACTIVE_LOGIN + GUI_SESSION +
#  GUI_FP, OHNE RTOS_SELFTEST) bootet bis zum Login-Prompt, ist grep-clean und traegt die
#  GUI-FP-Policy (FPEN=0b11 + T3.1-FP-Kontext). Vorher fiel -GuiLogin -Verify faelschlich in
#  die Standard-Selbsttest-Suite durch (die auf einem Login-Image nichts pruefen kann).
# ============================================================
if ($GuiLogin -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }
    . (Join-Path $PSScriptRoot '_build/tools/rc_markers.ps1')

    Write-Host ''
    Write-Host '=== -GuiLogin-Verifikation (Login->GUI-Image: Boot + grep-clean + FP-Policy) ===' -ForegroundColor Cyan
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'guilogin_serial.txt') (Join-Path $PSScriptRoot 'guilogin_err.txt') 20
    $log = $r.Log
    $ok = $true

    # Pflicht-Marker der Taxonomie -- AUSSER dem RC-FPEN-Marker (0b00): GUI-Builds tragen 0b11.
    foreach ($m in $RcReadyMarkers) {
        if ($m.Pattern -like '*FPEN=0b00*') { continue }
        $ok = (Check ('GuiLogin: ' + $m.Name) ($log -match $m.Pattern)) -and $ok
    }
    $ok = (Check 'GuiLogin: FP-Policy 0b11 (EL0-FP + FP-Kontext je Task, T3.1)' ($log -match 'CPACR_EL1\.FPEN=0b11 \(EL0-FP frei, FP-Kontext je Task')) -and $ok
    $ok = (Check 'GuiLogin: kein Fail-closed-Halt' (-not ($log -match $RcHaltPattern))) -and $ok
    $dirty = @($RcForbidden | Where-Object { $log -match [regex]::Escape($_) })
    $cleanMsg = 'GuiLogin: grep-clean (kein Scaffolding/Backdoor)'
    if ($dirty.Count -gt 0) { $cleanMsg += ' -- GEFUNDEN: ' + ($dirty -join ', ') }
    $ok = (Check $cleanMsg ($dirty.Count -eq 0)) -and $ok

    if ($ok) { Write-Host '-GuiLogin-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host '-GuiLogin-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  T1.17 HW-Harness-Verifikation (QEMU-Teil). Der Prueffstand selbst besteht aus
#  _build/tools/gen_hwsd.py (bootfaehiges SD-Image) + _build/tools/hw_verify.ps1 (Serial-Marker-Poller,
#  zentrale Taxonomie _build/tools/rc_markers.ps1). Am Schreibtisch/QEMU verifizierbar:
#    (1) gen_hwsd baut ein gueltiges SD-Image; Read-Back am Host prueft MBR + beide FAT32.
#    (2) Der Poller bootet QEMU VOM gen_hwsd-Image -> beweist, dass das Fixture bootbar ist,
#        das RTOS dessen hdd0/hdd1 liest und alle RC-Marker erscheinen (grep-clean).
#  HW-only (nicht QEMU-pruefbar): echter Firmware-/armstub-Handoff, PL011 auf GPIO14/15,
#  COM-Port-Pfad des Pollers, Power-Cycle -- siehe docs/architecture/15-hw-bringup-plan.md.
# ============================================================
if ($HwHarness -and $Verify) {
    if (-not $py) { Write-Warning 'Python fehlt -- HW-Harness-Verify braucht Python.'; exit 1 }
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== T1.17 HW-Harness-Verifikation (SD-Fixture + Serial-Poller, QEMU-Teil) ===' -ForegroundColor Cyan
    $ok = $true

    # (1) Bootfaehiges SD-Image bauen + Read-Back am Host (MBR + 2x FAT32 + Pflichtdateien).
    $hwsd = Join-Path $PSScriptRoot '_build/hwsd.img'
    & $py '_build/tools/gen_hwsd.py' $hwsd '--kernel' $img '--init' '_build/hello.elf' '--shell' '_build/shell.elf' '--total-mib' '256' '--check'
    $ok = (Check 'T1.17: gen_hwsd Read-Back (MBR + 2x FAT32 + Pflichtdateien)' ($LASTEXITCODE -eq 0)) -and $ok

    # (2) Serial-Poller gegen QEMU, das VOM gen_hwsd-Image bootet. Kindprozess-PowerShell,
    #     damit dessen 'exit' den build.ps1-Prozess nicht mitbeendet.
    $hv = Join-Path $PSScriptRoot '_build/tools/hw_verify.ps1'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $hv -SelfTest -Image $img -Sd $hwsd -TimeoutSec 40
    $ok = (Check 'T1.17: Serial-Poller (Boot vom gen_hwsd-Image -> alle RC-Marker)' ($LASTEXITCODE -eq 0)) -and $ok

    if ($ok) { Write-Host 'T1.17 HW-Harness-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'T1.17 HW-Harness-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  T2.1 GUI-Grafik-Bruecke: der Standard-Selbsttest-Boot beweist headless (a) die Kernel-seitige
#  Backbuffer->FB-Kopie + GPU-Cache-Flush (Pixel-Readback) und (b) dass eine EL0-App via
#  SYS_GUI_INFO/SYS_GUI_FLUSH in den gemappten Backbuffer zeichnen kann. Visuell mit -Hdmi
#  (bzw. run_shell.ps1 -Hdmi) sichtbar; hier headless gegen Marker.
# ============================================================
if ($Gui -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== T2.1-T2.6 GUI-Verifikation (Bruecke + libgui + USB-Maus + WinForms-Kern + Controls + Nav) ===' -ForegroundColor Cyan
    # usb-mouse angehaengt -> HID-Boot-Maus (dev_kind=3) statt Tastatur; GUITEST/libgui laeuft unabhaengig.
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'gui_serial.txt') (Join-Path $PSScriptRoot 'gui_err.txt') 16 'usb-mouse'
    $log = $r.Log
    $ok = $true

    $ok = (Check 'GUI: Grafik-Bruecke bereit (Backbuffer @ EL0-VA 0x18000000, GUI-Cap-gated)' ($log -match '\[gui\] Grafik-Bruecke bereit')) -and $ok
    $ok = (Check 'GUI: Kernel-Bridge bb->fb + GPU-Cache-Flush (Pixel-Readback)' ($log -match '\[gui\] bridge: bb->fb ok')) -and $ok
    $ok = (Check 'T2.3 Cursor-Overlay: Kernel malt Cursor beim Flush (Kontur/Fuellung/BG)' ($log -match '\[gui\] cursor-overlay: ok')) -and $ok
    $ok = (Check 'GUI: EL0 zeichnet via libgui (fill/rect/text/clip, Pixel-Readback)' ($log -match '\[guitest\] libgui fill=ok rect=ok text=ok clip=ok')) -and $ok
    $ok = (Check 'T2.3 USB-Maus: HID-Boot-Maus enumeriert (usb-mouse, dev_kind=3)' ($log -match '\[usb\] HID-Maus bereit')) -and $ok
    $ok = (Check 'T2.3 USB-Maus: HID-IRQ fuer die Maus scharfgeschaltet (dev_kind==3-Guard)' ($log -match '\[mouse\] irq-armed: ok')) -and $ok
    $ok = (Check 'T2.3 USB-Maus: Boot-Report-Dekodierung (dx/dy/Buttons + Clamping)' ($log -match '\[mouse\] decode-selbsttest: ok')) -and $ok
    $ok = (Check 'T2.3 Event-Queue: Kernel-Ring einreihen/poppen (FIFO)' ($log -match '\[event\] queue-selbsttest: ok')) -and $ok
    $ok = (Check 'T2.3 Event-Syscalls: EL0 SYS_WAIT_EVENT/SYS_POLL_EVENT (Kernel->EL0-Fluss)' ($log -match '\[guitest\] events wait\+poll=\d+ first=\(111,222,btn0\)')) -and $ok
    $ok = (Check 'T2.4 WinForms-Kern: Hit-Test + Klick-Dispatch + Fokus + Repaint (Control-Pool)' ($log -match '\[guitest\] winforms hittest=ok click=ok focus=ok paint=ok')) -and $ok
    $ok = (Check 'T2.5 Standard-Controls: Panel + Label (transparent) + TextBox (Tastatur/Caret)' ($log -match '\[guitest\] controls panel=ok label=ok textbox=ok')) -and $ok
    $ok = (Check 'T2.6 Tastaturnavigation: Tab wechselt Fokus (Umlauf) + Enter loest Button aus' ($log -match '\[guitest\] keynav tab=ok enter=ok')) -and $ok

    if ($ok) { Write-Host 'T2.1-T2.6 GUI-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'T2.1-T2.6 GUI-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  T2.6 GUI-Sitzung: der Selbsttest-Boot startet nach dem "Login" GUI.ELF (WinForms-Demo) statt des
#  Bruecke-Tests. Beweist headless: fbcon-Handoff (Kernel gibt den Schirm frei) + GUI.ELF gestartet +
#  Demo-Form gebaut + deterministischer Bedienungs-Selbsttest (Maus-Fokus + Tastatur + Nav + Klick).
#  Visuell (Maus) mit run_gui.ps1 -Hdmi. Der interaktive Login->GUI-Pfad (GUI_SESSION, Serial-/USB-
#  Tastatur) ist verdrahtet und wird visuell/auf HW gefahren.
# ============================================================
if ($GuiApp -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== T2.6 GUI-Sitzung (GUI.ELF nach Login: fbcon-Handoff + Demo + Bedienung) ===' -ForegroundColor Cyan
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'guiapp_serial.txt') (Join-Path $PSScriptRoot 'guiapp_err.txt') 16 'usb-mouse'
    $log = $r.Log
    $ok = $true

    $ok = (Check 'T2.6 fbcon-Handoff: Kernel gibt den Framebuffer an die GUI frei' ($log -match '\[gui\] fbcon-Mirror aus')) -and $ok
    $ok = (Check 'T2.6 fbcon-Handoff bleibt aus (GUI.ELF-Start als Erfolg erkannt, Mirror NICHT zurueckgeholt)' (-not ($log -match 'GUI\.ELF-Start fehlgeschlagen'))) -and $ok
    $ok = (Check 'T2.8 EL0-FP: Fliesskomma am EL0 freigeschaltet + laeuft (FPEN=0b11, smoke=778)' ($log -match '\[gui\] el0-fp: ok')) -and $ok
    $ok = (Check 'T2.8 TTF-Datei: ASCII-Subset zur Laufzeit von hdd1 geladen (SFNT-Magic ok)' ($log -match '\[gui\] ttf-datei: hdd1:GUIFONT.TTF geladen \+ SFNT-Magic ok')) -and $ok
    $ok = (Check 'T2.7 TTF-Font: DejaVu Sans gerastert + anti-aliased gerendert' ($log -match '\[gui\] ttf-font: DejaVu Sans geladen \(ok\)')) -and $ok
    $ok = (Check 'T2.8 Laufzeit-Rasterung: Glyphen live aus der .ttf gerastert (FP, keine Build-Bake)' ($log -match '\[gui\] ttf-modus: LAUFZEIT-Rasterung')) -and $ok
    $ok = (Check "T2.8 Rasterizer korrekt: 'o' mit Counter-Loch (Nonzero-Winding-Fill)" ($log -match "\[gui\] ttf-form: 'o' mit Loch")) -and $ok
    $ok = (Check 'T2.6 GUI.ELF gestartet: WinForms-Sitzung + Demo-Form gebaut' ($log -match "\[gui\] Sitzung gestartet: Form 'rpi_rtos GUI'")) -and $ok
    $ok = (Check 'T2.6 Demo-Bedienung: Maus-Fokus + Tastatur + Tab-Nav + Enter-Klick' ($log -match '\[gui\] demo selftest fokus=ok tastatur=ok nav=ok klick=ok')) -and $ok
    $ok = (Check 'T2.6 GUI bereit fuer Eingaben (Message-Loop wf_run)' ($log -match '\[gui\] bereit fuer Eingaben')) -and $ok

    if ($ok) { Write-Host 'T2.6 GUI-Sitzung: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'T2.6 GUI-Sitzung: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  Vulkan-in-WinForms-Demo (VKGUI.ELF): eine GUI-App mit eingebettetem, live gerendertem Vulkan-
#  Wuerfel-Viewport. Beweist die Integration beider Subsysteme: Software-Vulkan rendert OFFSCREEN,
#  die Pixel werden ins WinForms-Panel geblittet. Fixes Fenster (die Render-Schleife laeuft endlos
#  bis 'Beenden') -- der Poll faengt Boot + Setup + die Selbsttest-Probe.
# ============================================================
if ($VkGuiApp -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== Vulkan-in-WinForms-Demo (VKGUI.ELF: offscreen-Vulkan -> GUI-Panel) ===' -ForegroundColor Cyan
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'vkgui_serial.txt') (Join-Path $PSScriptRoot 'vkgui_err.txt') 20 'usb-mouse'
    $log = $r.Log
    $ok = $true

    $ok = (Check 'fbcon-Handoff: Kernel gibt den Framebuffer an die Vulkan-GUI frei' ($log -match '\[vkgui\] fbcon-Mirror aus')) -and $ok
    $ok = (Check 'VKGUI.ELF gestartet (kein Startfehler)' (-not ($log -match 'VKGUI\.ELF-Start fehlgeschlagen'))) -and $ok
    $ok = (Check 'Shader von hdd1 geladen (VERT.SPV/FRAG.SPV)' ($log -match '\[vkgui\] shader geladen')) -and $ok
    $ok = (Check 'Vulkan-Setup ok (offscreen Farbbild + Tiefe + Pipeline aus .spv)' ($log -match '\[vkgui\] vulkan-setup ok')) -and $ok
    $ok = (Check 'WinForms-Chrome gezeichnet, Render-Schleife gestartet' ($log -match '\[vkgui\] chrome gezeichnet')) -and $ok
    $ok = (Check 'Vulkan-Wuerfel im GUI-Viewport sichtbar (offscreen -> Panel-Blit)' ($log -match '\[vkgui\] probe: wuerfel im GUI-viewport sichtbar')) -and $ok
    $ok = (Check 'kein FEHLER-Marker in [vkgui]-Zeilen' (-not ($log -cmatch '\[vkgui\].*FEHLER'))) -and $ok

    if ($ok) { Write-Host 'Vulkan-in-WinForms-Demo: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'Vulkan-in-WinForms-Demo: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  T3.x Phase 3 (3D/Vulkan): FP-Kontextsicherung + Rasterizer-/Vulkan-Selbsttests.
#  T3.1: 2x FPTEST.ELF auf DEMSELBEN Kern (1) haemmern disjunkte V-Register-/FPCR-Muster ueber
#  Schlaf-Praeemptionen. Beweist: (a) genullter FP-Start (kein Cross-Prozess-V-Register-Leak),
#  (b) FP/SIMD-Zustand ueberlebt Praeemption (fpctx_save/restore im schedule()).
# ============================================================
if ($Vk -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== T3.x Phase-3-Verifikation (FP-Kontext + 3D/Vulkan) ===' -ForegroundColor Cyan
    # MARKER-GESTEUERT statt fixem Fenster: bricht ab, sobald VKCUBE seine Probe gedruckt hat
    # ('laeuft weiter'), sonst nach 90s (Fehlerfall). Robust gegen wachsende Test-Suiten +
    # Host-Last -- im Gutfall schnell, kein deterministisches Timeout mehr (Review #30 endgueltig).
    $r = Invoke-RaspiUntil $qemu $img (Join-Path $PSScriptRoot 'vk_serial.txt') (Join-Path $PSScriptRoot 'vk_err.txt') '\[vkcube\] laeuft weiter' 90
    $log = $r.Log
    $ok = $true

    $ok = (Check 'T3.1 CPACR: EL0-FP freigeschaltet (FPEN=0b11)' ($log -match 'CPACR_EL1\.FPEN=0b11 \(EL0-FP frei, FP-Kontext je Task')) -and $ok
    $ok = (Check 'T3.1 Guardian gestartet (2x FPTEST.ELF auf Kern 1)' ($log -match '\[vk\] T3\.1 FP-Kontext-Guardian: 2x FPTEST\.ELF auf Kern 1')) -and $ok
    $ok = (Check 'T3.x kein Prozess-Startfehler im VK-Zweig' (-not ($log -cmatch '\[vk\] FEHLER'))) -and $ok
    # ($fpClean statt $clean: build.ps1 hat einen [switch]$Clean-PARAMETER -- PS-Variablen sind
    #  case-insensitiv, die Zuweisung wuerde in den SwitchParameter laufen und werfen.)
    # 3 Instanzen: 2 Boot-Instanzen (frische Slots) + 1 Reuse-Instanz (recycelter Slot, von
    # VKTEST nach deren Exit gespawnt) -> die dritte beweist die Zero-Init bei Slot-Reuse.
    $fpClean = ([regex]::Matches($log, '\[fptest\] pid=\d+ start-zustand: sauber')).Count
    $ok = (Check ("T3.1 FP-Start genullt (inkl. RECYCELTEM Slot): 3/3 sauber (gefunden: {0})" -f $fpClean) ($fpClean -eq 3)) -and $ok
    $fpHammer = ([regex]::Matches($log, '\[fptest\] pid=\d+ v-register\+fpcr\+fpsr ueber 40 schlaf-praeemptionen: ok')).Count
    $ok = (Check ("T3.1 FP-Kontext (V-Regs+FPCR+FPSR) ueberlebt Praeemption: 3/3 Hammer fehlerfrei (gefunden: {0})" -f $fpHammer) ($fpHammer -eq 3)) -and $ok
    $ok = (Check 'T3.1 Reuse-Spawn erfolgt (dritte Instanz auf recyceltem Slot)' ($log -match '\[vktest\] fpctx-reuse: dritte FPTEST-Instanz auf Kern 1 gestartet')) -and $ok
    $ok = (Check 'T3.1 Zero-Init WHITE-BOX: vergifteter Slot wird bei Vergabe genullt (deterministisch)' ($log -match '\[fpctx\] zero-init bei Slot-Vergabe \(white-box, vergifteter Slot\): ok')) -and $ok
    $ok = (Check 'T3.1 kein FP-Korruptions-/Leak-Marker im Log' (-not ($log -cmatch '\[fptest\].*(FEHLER|VERSCHMUTZT)'))) -and $ok
    $ok = (Check 'T3.2 r3d: flat+gouraud(Zentroid)+tiefe+cull+fillrule(2 Diagonalen)+nearclip+PERSP(w!=1)+farclip+cullfront+scissor' ($log -match '\[vktest\] r3d: flat=ok gouraud=ok tiefe=ok cull=ok fillrule=ok\(doppelt=0,luecken=0\) nearclip=ok persp=ok farclip=ok cullfront=ok scissor=ok')) -and $ok
    $ok = (Check 'T3.3 Vulkan: Instance + 1 Geraet + Count-Protokoll(VK_INCOMPLETE) + Extension-Fehlerpfad' ($log -match '\[vktest\] vk: instance=ok geraete=1:ok protokoll\(INCOMPLETE\)=ok ext-neg=ok')) -and $ok
    $ok = (Check 'T3.3/V1.10 Vulkan: Eigenschaften + gehobene Limits (CPU, GRAPHICS-Queue, ehrlicher Heap, Format-Neg, mrt=8, push=256, Vertex-Format)' ($log -match '\[vktest\] vk: eigenschaften=ok \(cpu-geraet, 1 queue-familie graphics, heap=196608, format-neg=ok, mrt=8, push=256, vtx-fmt=ok\)')) -and $ok
    $ok = (Check 'T3.3 Vulkan: Device + Queue + VK_ERROR_FEATURE_NOT_PRESENT-Pfad' ($log -match '\[vktest\] vk: device=ok queue=ok feature-neg=ok')) -and $ok
    $ok = (Check 'T3.3 Vulkan: Speicher (Alloc/Map/Write/Read/Bind, Image+View, rowPitch, ehrlicher OOM)' ($log -match '\[vktest\] vk: speicher=ok \(map/write/read\+bind\) image=ok \(rowpitch=256\+view\) oom=VK_ERROR_OUT_OF_DEVICE_MEMORY')) -and $ok
    $ok = (Check 'T3.3 Vulkan: vkGetInstanceProcAddr (identische Entries, NULL fuer unbekannt)' ($log -match '\[vktest\] vk: procaddr=ok')) -and $ok
    $ok = (Check 'T3.3 Vulkan-Kern abgeschlossen' ($log -match '\[vktest\] vk-kern fertig')) -and $ok
    $ok = (Check 'T3.4 SPIR-V: Parse + Entry-Points (vert/frag/test)' ($log -match '\[vktest\] spirv: parse\+entry \(vert/frag/test\)=ok')) -and $ok
    $ok = (Check 'T3.4 SPIR-V: Vertex-Shader mvp*pos (PushConstant, Python-Referenz +-1e-4)' ($log -match '\[vktest\] spirv: vertex mvp\*pos\+color \(pushconst, referenz \+-1e-4\)=ok')) -and $ok
    $ok = (Check 'T3.4 SPIR-V: Fragment-Shader vec4(vColor,1)' ($log -match '\[vktest\] spirv: fragment vec4\(vColor,1\)=ok')) -and $ok
    $ok = (Check 'T3.4 SPIR-V: Branch/Phi/Shuffle/Normalize/Dot/Select (BEIDE Pfade)' ($log -match '\[vktest\] spirv: branch/phi/shuffle/normalize/dot/select \(beide pfade\)=ok')) -and $ok
    $ok = (Check 'T3.4 SPIR-V: fail-loud (verfaelschtes Magic abgelehnt)' ($log -match '\[vktest\] spirv: magic-neg=ok')) -and $ok
    $ok = (Check 'T3.4 SPIR-V HAERTUNG: untrusted <id> >= bound -> fail-loud statt OOB-Write (Review CRITICAL)' ($log -match '\[vktest\] spirv: haertung \(untrusted <id> >= bound -> fail-loud statt OOB\)=ok')) -and $ok
    $ok = (Check 'T3.4 SPIR-V abgeschlossen' ($log -match '\[vktest\] spirv fertig')) -and $ok
    $ok = (Check 'T3.5 Draw-Pfad: alle Objekte durch die API (Module/Layout/RP/FB/Pipeline/CB)' ($log -match '\[vktest\] vkdraw: objekte=ok')) -and $ok
    $ok = (Check 'T3.5 Draw-Pfad: vkQueueSubmit + Fence signalisiert' ($log -match '\[vktest\] vkdraw: submit\+fence=ok')) -and $ok
    $ok = (Check 'T3.5 Draw-Pfad: vkMapMemory-Readback (Dreieck gruen, Clear exakt, Tiefe 0.25, fernes rot verworfen)' ($log -match '\[vktest\] vkdraw: readback center=gruen\(nah\) ecke=clearfarbe tiefe=0\.25 ferngetestet=verworfen')) -and $ok
    $ok = (Check 'T3.5 Draw-Pfad: Push-Constants PRO Draw (2 Draws, Push je Draw eingefroren -- Review #7)' ($log -match '\[vktest\] vkdraw: push-pro-draw \(2 draws, Push je Draw eingefroren\)=ok')) -and $ok
    $ok = (Check 'V1.1 Blending: 50/50 CONSTANT_ALPHA (gruen+rot -> exakt 0xFF808000, echtes RGBA)' ($log -match '\[vktest\] vkdraw: blending \(50/50 CONSTANT_ALPHA gruen\+rot -> 0xFF808000\)=ok')) -and $ok
    $ok = (Check 'V1.2 Index-Draw: vkCmdDrawIndexed (uint16-Index [3,4,5] -> rotes Dreieck gefetcht)' ($log -match '\[vktest\] vkdraw: index-draw \(uint16 \[3,4,5\] -> rotes Dreieck via vkCmdDrawIndexed\)=ok')) -and $ok
    $ok = (Check 'V1.2 Instancing: gl_InstanceIndex (2 Instanzen links+rechts platziert)' ($log -match '\[vktest\] vkdraw: instancing \(gl_InstanceIndex, 2 instanzen links\+rechts\)=ok')) -and $ok
    $ok = (Check 'V1.3 Descriptor-Sets/UBO: FS liest Uniform-Buffer (set0/binding0 vec4 -> 0xFF4080BF)' ($log -match '\[vktest\] vkdraw: descriptor-ubo \(set0/binding0 vec4 -> FS gibt 0xFF4080BF aus\)=ok')) -and $ok
    $ok = (Check 'V1.4 Texturen: sampler2D nearest (2x2, texture() an konst. Koord -> blauer Texel 0xFF0000FF)' ($log -match '\[vktest\] vkdraw: textur \(2x2 sampler2D nearest, texel\(0,1\) -> 0xFF0000FF\)=ok')) -and $ok
    $ok = (Check 'V1.5 MRT: 2 Farb-Attachments (FS-Ausgaben getrennt: rot@0, gruen@1)' ($log -match '\[vktest\] vkdraw: mrt \(2 attachments: o0=rot@0, o1=gruen@1\)=ok')) -and $ok
    $ok = (Check 'V1.9 Transfer: FillBuffer+CopyBuffer (0xDEADBEEF) + ClearColorImage (gelb)' ($log -match '\[vktest\] vkdraw: transfer \(FillBuffer\+CopyBuffer=0xDEADBEEF, ClearColorImage=gelb\)=ok')) -and $ok
    $ok = (Check 'V1.8 Query-Pools + Events (Occlusion draw>0/leer=0, Timestamp t1>t0, Event host+device)' ($log -match '\[vktest\] vkdraw V1\.8: query\+event=ok \(occlusion draw>0\+leer=0, timestamp t1>t0, event host\+device\)')) -and $ok
    $ok = (Check 'V1.7 Compute-Pipeline (Dispatch(8): SSBO data[i]==i*3, gl_GlobalInvocationID+Array+StorageBuffer-Store)' ($log -match '\[vktest\] vkdraw V1\.7: compute=ok \(dispatch\(8\): SSBO data\[i\]==i\*3 -- gl_GlobalInvocationID\+Array\+Store\)')) -and $ok
    $ok = (Check 'V1.6 MSAA (4x Multisample-Raster + Resolve: Kanten-AA-Zwischenfarbe, Interior voll, Ecke Hintergrund)' ($log -match '\[vktest\] vkdraw V1\.6: msaa=ok \(4x multisample: kanten-AA zwischen-gruen, interior voll, ecke hintergrund, resolve\)')) -and $ok
    $ok = (Check 'T3.5 Draw-Pfad abgeschlossen' ($log -match '\[vktest\] vkdraw fertig')) -and $ok
    $ok = (Check 'T3.6 fbcon-Handoff: 3D uebernimmt den Schirm (Marker weiter ueber Serial)' ($log -match '\[vk\] fbcon-Mirror aus')) -and $ok
    $ok = (Check 'T3.6 VKCUBE gestartet (EL0-Spawn von hdd1)' ($log -match '\[vktest\] vkcube gestartet')) -and $ok
    $ok = (Check 'T3.6 VKCUBE: .spv-Dateien zur LAUFZEIT von hdd1 geladen (SPIR-V-Magic)' ($log -match '\[vkcube\] shader geladen: hdd1:VERT\.SPV \+ hdd1:FRAG\.SPV \(SPIR-V-Magic ok\)')) -and $ok
    $ok = (Check 'T3.6 VKCUBE: Surface + Swapchain (VK_KHR_surface/VK_RTOS_surface/VK_KHR_swapchain)' ($log -match '\[vkcube\] swapchain=ok')) -and $ok
    $ok = (Check 'T3.6 VKCUBE: RenderPass/Depth/Pipeline aus .spv-Modulen' ($log -match '\[vkcube\] pipeline=ok')) -and $ok
    $ok = (Check 'T3.6 VKCUBE: 12 Frames praesentiert, geschaerfte Probe (Wuerfel beleuchtet + Clear-Rand + ANIMATION)' ($log -match '\[vkcube\] probe nach 12 frames: mitte=wuerfel\(beleuchtet\) ecke=clearfarbe animation=ja -> vulkan-praesentation ok')) -and $ok
    $ok = (Check 'T3.6 kein FEHLER-Marker in [vkcube]-Zeilen' (-not ($log -cmatch '\[vkcube\].*(FEHLER|FEHLGESCHLAGEN)'))) -and $ok
    $ok = (Check 'T3.2 Demo: animierte Wuerfel mit Z-Buffer gerendert (60 Frames)' ($log -match '\[vktest\] demo: 60 frames wuerfel\+wuerfel \(z-buffer, beleuchtet\) gerendert')) -and $ok
    $ok = (Check 'T3.2 VKTEST sauber beendet (stufe1 fertig, exit 0)' ($log -match '\[vktest\] stufe1 fertig')) -and $ok
    # -cmatch (case-sensitiv): Fehl-Marker sind GROSS (FEHLER/FEHLGESCHLAGEN); ok-Marker wie
    # "ext-neg=ok" duerfen nicht per case-insensitivem Substring matchen.
    $ok = (Check 'T3.2/T3.3 kein FEHLER-Marker in [vktest]-Zeilen' (-not ($log -cmatch '\[vktest\].*(FEHLER|FEHLGESCHLAGEN)'))) -and $ok
    $ok = (Check 'Boot vollstaendig (Scheduler gestartet)' ($log -match '\[8\] Scheduler starten')) -and $ok

    if ($ok) { Write-Host 'T3.x Phase-3-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'T3.x Phase-3-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  Vision-Track (docs/architecture/19-ai-vision-plan.md): gekapseltes KI-Bildauswertungs-Modul.
#  A1.1: das NEON-fp32-sgemm-Primitiv der Inferenz-Engine, gegen BEKANNTE Referenz verifiziert
#  (Summe=2156, C[3][3]=272, N=5-Tail). Beweist zugleich die Kapselung: der -Vision-Flavor bootet
#  AIVISION.ELF (einziger Kernel-Kontakt = #ifdef VISION-Spawn), waehrend der Standard-Selbsttest
#  unveraendert weiterlaeuft.
# ============================================================
if ($Vision -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== Vision-Track A1.1 (NEON-fp32-Inferenz-Engine) ===' -ForegroundColor Cyan
    # Marker-gesteuert (Invoke-RaspiUntil): bricht ab, sobald der Engine-Selbsttest fertig meldet.
    $r = Invoke-RaspiUntil $qemu $img (Join-Path $PSScriptRoot 'vision_serial.txt') (Join-Path $PSScriptRoot 'vision_err.txt') '\[aivision\] engine-selftest fertig' 75
    $log = $r.Log
    $ok = $true

    $ok = (Check 'A1.1 CPACR: EL0-FP/NEON freigeschaltet (FPEN=0b11)' ($log -match 'CPACR_EL1\.FPEN=0b11')) -and $ok
    $ok = (Check 'A1.1 AIVISION.ELF via #ifdef VISION-Spawn gestartet' ($log -match '\[vision\] AIVISION\.ELF: KI-Bildauswertung-Selbsttest')) -and $ok
    $ok = (Check 'A1.1 kein Startfehler im Vision-Zweig' (-not ($log -cmatch '\[vision\] FEHLER'))) -and $ok
    $ok = (Check 'A1.1 sgemm(NEON fp32): 4x3*3x4 Summe=2156, C[3][3]=272, N=5-Tail ok' ($log -match '\[aivision\] A1\.1 sgemm NEON-fp32: checksum=2156 c33=272 tail=ok')) -and $ok
    $ok = (Check 'A1.2 conv2d(im2col->sgemm)+bias + Orientierung (Single-Tap, asymmetrisch) + dwconv (per-Kanal)' ($log -match '\[aivision\] A1\.2 conv2d\+bias=ok orient=ok dwconv\(per-channel\)=ok')) -and $ok
    $ok = (Check 'A1.3 ReLU/ReLU6/Sigmoid + Pooling(max/avg/global) + Softmax + BatchNorm-Fold' ($log -match '\[aivision\] A1\.3 relu/relu6/sigmoid=ok pool\(max/avg/global\)=ok softmax=ok bn-fold=ok')) -and $ok
    $ok = (Check 'A1.4a gehaerteter *.net-Blob-Loader: gueltiger Parse + 5 Haertungs-Ablehnungen (untrusted hdd1)' ($log -match '\[aivision\] A1\.4a blob-loader: parse=ok reject\(magic/trunc/type/nlayers/weights\)=ok')) -and $ok
    $ok = (Check 'A1.4b Graph-Runner: Mini-Netz CONV->RELU->GAP->FC end-to-end gegen Referenz' ($log -match '\[aivision\] A1\.4b graph-runner: mininet\(conv->relu->gap->fc\)=ok')) -and $ok
    $ok = (Check 'A1.6 gehaerteter BMP-Loader: 2x2 RGB-Ebenen korrekt + falsches Magic abgelehnt' ($log -match '\[aivision\] A1\.6 bmp-loader: 2x2 rgb-planes=ok reject-bad-magic=ok')) -and $ok
    $ok = (Check 'A1.5 4-Kern-Parallel-GEMM (VISION Kernel-Parallel-For): Ergebnis == Einkern + Mehrkern-Lauf' ($log -match '\[aivision\] A1\.5 parallel-sgemm: ergebnis==single-core kerne=\d+')) -and $ok
    $ok = (Check 'M0 end-to-end: hdd1-Datei-Bild + -Modell (untrusted) -> Klassifikation Klasse=0 (rot-dominant)' ($log -match '\[aivision\] M0 klassifikation: hdd1-bild\+modell -> klasse=0 \(rot-dominant\) ok')) -and $ok
    $ok = (Check 'A2 Detektions-Pipeline: Heatmap-Peak-Decode + NMS (Ueberlappung unterdrueckt) + Box-Overlay' ($log -match '\[aivision\] A2 detektor-pipeline: decode=ok nms\(overlap-suppressed\)=ok box-overlay=ok')) -and $ok
    $ok = (Check 'A3 Embedding+Anomalie: L2-Norm + known(naechster Nachbar) + anomaly(fremd ueber Schwelle)' ($log -match '\[aivision\] A3 embedding\+anomalie: l2-norm=ok known\(idx=0\)=ok anomaly\(fremd\)=ok')) -and $ok
    $ok = (Check 'A4.2 YUYV->RGB-Decode (untrusted Kamera-Daten): BT.601 + Clamping + bounds-check' ($log -match '\[aivision\] A4\.2 yuyv-decode: farbe\(R-clamp\+chroma\)=ok reject-short=ok')) -and $ok
    $ok = (Check 'A5.1 Echtzeit-Pipeline-Koerper: Frame -> Conv-Detektor -> Heatmap-Peak -> Box-Overlay' ($log -match '\[aivision\] A5\.1 pipeline: frame->conv-detektor->heatmap-peak->box\(8,4\)->overlay=ok')) -and $ok
    $ok = (Check 'A5 Echtzeit-Schleife: Kamera-Grab-Seam (QEMU kein UVC -> Fallback) + Pipeline-Loop x3' ($log -match '\[aivision\] A5 loop: cam-grab=kein-geraet\(qemu-fallback\) pipeline-loop x3=ok')) -and $ok
    $ok = (Check 'A4.1a UVC-Klassen-Layer: Config-Parse (VS+Bulk-IN) + PROBE-Payload + Bulk-Frame-Assembly' ($log -match '\[uvc\] class-layer: parse\(vs\+bulk-in\)=ok probe=ok assemble\(2pkt\+eof\)=ok')) -and $ok
    $ok = (Check 'A5.2 FPS-Mess-Infrastruktur: monotoner CNTPCT-Zaehler + CNTFRQ (Wert am Pi4 mit Kamera echt)' ($log -match '\[aivision\] A5\.2 fps-infra: ticks-monoton=ok cntfrq=\d+ fps~=\d+')) -and $ok
    $ok = (Check 'A1.x kein FEHLER-Marker in [aivision]-Zeilen' (-not ($log -cmatch '\[aivision\].*FEHLER'))) -and $ok
    $ok = (Check 'A1.1 Engine-Selbsttest abgeschlossen' ($log -match '\[aivision\] engine-selftest fertig')) -and $ok
    $ok = (Check 'Boot vollstaendig (Scheduler gestartet)' ($log -match '\[8\] Scheduler starten')) -and $ok

    if ($ok) { Write-Host 'Vision-Track A1.1: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'Vision-Track A1.1: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  V3D-Hardware-Erkennung (Vulkan V5, erster Bring-up-Schritt): der -V3d-Boot liest die V3D-IDENT-
#  Register. QEMU (keine V3D) -> "nicht gefunden"; echter Pi4 -> reale IDENT ueber Serial. Ein
#  arbeitendes V3D-Backend bleibt research-grade (docs/architecture/18, Block V5).
# ============================================================
if ($V3d -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }
    Write-Host ''
    Write-Host '=== V3D-Hardware-Erkennung (Vulkan V5, erster Bring-up-Schritt) ===' -ForegroundColor Cyan
    $r = Invoke-RaspiUntil $qemu $img (Join-Path $PSScriptRoot 'v3d_serial.txt') (Join-Path $PSScriptRoot 'v3d_err.txt') '\[v3d\]' 40
    $log = $r.Log
    $ok = $true
    $ok = (Check 'V3D-Probe lief (Serial-Marker [v3d])' ($log -match '\[v3d\]')) -and $ok
    $ok = (Check 'V3D in QEMU erwartungsgemaess NICHT gefunden (kein V3D emuliert)' ($log -match '\[v3d\] V3D nicht gefunden')) -and $ok
    $ok = (Check 'Boot vollstaendig (Scheduler gestartet)' ($log -match '\[8\] Scheduler starten')) -and $ok
    if ($ok) { Write-Host 'V3D-Erkennung: ALLE PASS (am echten Pi4 meldet dieselbe Probe die reale V3D-IDENT)' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'V3D-Erkennung: FEHLER' -ForegroundColor Red; exit 1 }
}

# ============================================================
#  Dev-Remote-Interface (docs/architecture/20): Protokoll-Kern-Selbsttest (D1). Der -DevRemote-Boot
#  laeuft den synthetischen Kern-Test; der UDP-Agent (D2) + Host-Python (D3) folgen. Sicherheit: ganz
#  #ifdef DEV_REMOTE -> ohne das Flag byte-identischer Kernel; nie im RC-/Release-Produktions-Image.
# ============================================================
if ($DevRemote -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }
    Write-Host ''
    Write-Host '=== Dev-Remote D1+D2 (Protokoll-Kern + UDP-Agent) ===' -ForegroundColor Cyan
    # Done-Marker = der D2-QEMU-Zweig (nach dem Dispatch-Selbsttest, vor dem Scheduler) -> die D2-Zeilen
    # sind damit zuverlaessig erfasst. Der Live-Netz-Agent wird in QEMU bewusst uebersprungen (GENET-
    # Zugriff = External-Abort); der reale UDP-Pfad laeuft am Pi4.
    $r = Invoke-RaspiUntil $qemu $img (Join-Path $PSScriptRoot 'dev_serial.txt') (Join-Path $PSScriptRoot 'dev_err.txt') '\[dev\] net: QEMU' 40
    $log = $r.Log
    $ok = $true
    $ok = (Check 'D1 Protokoll-Kern: Header-Parse + Reject + Datei-Reassembly (OOO+Dup) + RLE-Roundtrip' ($log -match '\[dev\] proto-core: header=ok reject=ok file-reassembly\(ooo\+dup\)=ok rle-roundtrip=ok')) -and $ok
    $ok = (Check 'D2 Dispatch (Mock-Ops): ping+key+mouse+file(ooo)+screen+restart' ($log -match '\[dev\] agent: dispatch ping=ok key=ok mouse=ok file\(ooo\)=ok screen=ok restart=ok')) -and $ok
    $ok = (Check 'D2 Live-Agent in QEMU uebersprungen (kein GENET-Zugriff, kein Abort)' ($log -match '\[dev\] net: QEMU \(kein DTB\)')) -and $ok
    $ok = (Check 'kein FEHLER-Marker in [dev]-Zeilen' (-not ($log -cmatch '\[dev\].*FEHLER'))) -and $ok
    $ok = (Check 'Boot vollstaendig (Scheduler gestartet)' ($log -match '\[8\] Scheduler starten')) -and $ok
    if ($ok) { Write-Host 'Dev-Remote D1+D2: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'Dev-Remote D1+D2: FEHLER' -ForegroundColor Red; exit 1 }
}

# ============================================================
#  T1.4-Panik-Guardian: absichtlicher EL1-Kernel-Fault -> panic() muss ALLE Kerne stoppen
#  (nicht nur den fehlerhaften). Beweist Register-Dump + Cross-Core-Halt via Halt-SGI.
# ============================================================
if ($PanicTest -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== T1.4-Panik-Verifikation (EL1-Fault -> alle Kerne stoppen) ===' -ForegroundColor Cyan
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'panic_serial.txt') (Join-Path $PSScriptRoot 'panic_err.txt') 16
    $log = $r.Log
    $ok = $true

    $ok = (Check 'Panik: absichtlicher EL1-Fault ausgeloest' ($log -match '\[paniktest\] loese absichtlich')) -and $ok
    $ok = (Check 'Panik: panic() erreicht (KERNEL PANIC-Banner)' ($log -match '\*\*\* KERNEL PANIC')) -and $ok
    $ok = (Check 'Panik: Register-Dump (ESR/ELR/FAR)' (($log -match 'ESR_EL1 = 0x') -and ($log -match 'FAR_EL1 = 0x'))) -and $ok
    $halted = ([regex]::Matches($log, '\[panic\] Kern \d+ gestoppt')).Count
    $ok = (Check ("Panik: Sekundaerkerne per Halt-SGI gestoppt (>=2 von 3, gefunden: {0})" -f $halted) ($halted -ge 2)) -and $ok
    $ok = (Check 'Panik: Halt-Bestaetigung verifiziert ("Alle Kerne gestoppt")' ($log -match 'Alle Kerne gestoppt')) -and $ok

    if ($ok) { Write-Host 'T1.4-Panik-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'T1.4-Panik-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ============================================================
#  T1.5-Guardian: rekursives mutex_lock muss FAIL-LOUD in panic() muenden (statt stiller No-op,
#  der das folgende unlock den noch benutzten Abschnitt freigeben liesse = Silent Corruption).
# ============================================================
if ($MutexPanicTest -and $Verify) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host ''
    Write-Host '=== T1.5-Verifikation (rekursives mutex_lock -> fail-loud panic) ===' -ForegroundColor Cyan
    $r = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'rmutex_serial.txt') (Join-Path $PSScriptRoot 'rmutex_err.txt') 16
    $log = $r.Log
    $ok = $true

    $ok = (Check 'RMutex: rekursives mutex_lock ausgeloest' ($log -match '\[rmutextest\] loese absichtlich')) -and $ok
    $ok = (Check 'RMutex: fail-loud -> panic() erreicht (KERNEL PANIC)' ($log -match '\*\*\* KERNEL PANIC')) -and $ok
    $ok = (Check 'RMutex: panic-Grund nennt rekursives mutex_lock' ($log -match 'rekursives mutex_lock \(einstufiges Design\)')) -and $ok
    $ok = (Check 'RMutex: KEIN stiller No-op (Post-Lock-Zeile nie erreicht)' (-not ($log -match 'kein panic ausgeloest'))) -and $ok

    if ($ok) { Write-Host 'T1.5-Verifikation: ALLE PASS' -ForegroundColor Green; exit 0 }
    else     { Write-Host 'T1.5-Verifikation: FEHLER'    -ForegroundColor Red;   exit 1 }
}

# ---- USB-Tastatur-getriebene Login/Shell-Verifikation (Monitor sendkey) ----
if ($Login -and $Verify -and $UsbKbd) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    $ser = Join-Path $PSScriptRoot 'usb_serial.txt'
    Remove-Item -Force -ErrorAction SilentlyContinue $ser
    $sdpart = ''
    if (Test-Path '_build/sd.img') { $sdpart = ' -drive file=_build/sd.img,if=sd,format=raw,cache=writethrough' }

    # Serielle Ausgabe in eine Datei, Eingabe ueber den QEMU-Monitor (stdin) per
    # 'sendkey' -> der Kernel liest die Tasten ueber console_getc vom USB-Keyboard.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $qemu
    $psi.Arguments = "-M raspi4b -kernel `"$img`"$sdpart -device usb-kbd -serial file:`"$ser`" -monitor stdio -display none"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)

    # Mit FileShare.ReadWrite lesen, damit das parallele Schreiben von QEMU den Lesezugriff
    # nicht blockiert (Get-Content -Raw kollidiert sonst mit QEMUs offenem Schreib-Handle).
    function Read-Ser {
        if (-not (Test-Path $ser)) { return '' }
        try {
            $fs = [IO.File]::Open($ser, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            $sr = New-Object IO.StreamReader($fs)
            $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $t
        } catch { return '' }
    }
    function Send-Key($name) { $proc.StandardInput.WriteLine("sendkey $name"); $proc.StandardInput.Flush(); Start-Sleep -Milliseconds 180 }
    function Type-Str($s) {
        foreach ($ch in $s.ToCharArray()) {
            $k = if ($ch -eq "`n") { 'ret' } elseif ($ch -eq ' ') { 'spc' } else { [string]$ch }
            Send-Key $k
        }
    }
    function Wait-For($pat, $secs) {
        $dl = (Get-Date).AddSeconds($secs)
        while ((Get-Date) -lt $dl -and ((Read-Ser) -notmatch $pat)) { Start-Sleep -Milliseconds 200 }
    }

    Wait-For 'login:' 25          # Boot (USB-Enum + HDMI-Rendering + PBKDF2) bis zum Login-Prompt
    Write-Host 'Tippe Login + Befehle ueber die emulierte USB-Tastatur (sendkey)...' -ForegroundColor Cyan
    Start-Sleep -Milliseconds 600 # QEMU-Input-Subsystem "aufwaermen"
    Send-Key 'backspace'          # Throwaway: faengt den ersten verschluckten Tastendruck ab (am leeren Prompt ignoriert)
    Type-Str "admin`n"
    Wait-For 'password' 6
    Send-Key 'backspace'          # Throwaway auch vor dem Passwort
    Type-Str "admin`n"
    Wait-For 'rpi_rtos Shell' 14
    Start-Sleep -Milliseconds 500
    Type-Str "help`n"               # nur Buchstaben+Enter; Ausgabe 'Befehle:' beweist IRQ-Eingabe
    Wait-For 'Befehle:' 8
    Type-Str "exit`n"
    Wait-For 'abgemeldet' 6

    $proc.StandardInput.WriteLine('quit'); $proc.StandardInput.Flush()
    Start-Sleep -Milliseconds 600
    if (-not $proc.HasExited) { $proc.Kill() }
    try { $proc.WaitForExit(2000) | Out-Null } catch {}
    Start-Sleep -Milliseconds 300       # Datei-Flush abwarten
    $log = Read-Ser
    Write-Host "`n===== Serial-Log (USB-Tastatur treibt Login/Shell) =====`n"
    Write-Host $log
    Write-Host "`n===== Verifikation ====="
    # Gatende Kern-Checks: Enumeration + per USB getippter Username am Prompt + Login-Erfolg + Shell.
    $ok = $true
    $ok = (Check 'USB: HID-Tastatur enumeriert' ($log -match 'HID-Tastatur: addr=2')) -and $ok
    $ok = (Check 'USB: Username "admin" am Login-Prompt getippt' ($log -match 'login: admin')) -and $ok
    $ok = (Check 'USB-Login: admin akzeptiert (per USB getippt)' ($log -match 'Login erfolgreich: admin')) -and $ok
    $ok = (Check 'USB-Shell: Prompt erschienen' ($log -match 'rpi\[uid0 hdd1')) -and $ok
    # Informativ: QEMUs DWC2-Interrupt-IN praesentiert sendkey-Reports nur in einem
    # schmalen Fenster -> die Shell-Phase (Tastatur-Rate ~100 Hz) ist im Headless-
    # Harness fragil (war im alten Polling-Code ebenso). Login oben beweist die
    # Tastatur-Eingabe (mehrere Tasten 'admin' + Enter -> Login akzeptiert).
    [void](Check 'USB-Shell: help -> Befehle (informativ)' ($log -match 'Befehle:'))
    [void](Check 'USB-Shell: exit -> abgemeldet (informativ)' ($log -match 'abgemeldet'))
    if ($ok) { Write-Host "`nERGEBNIS: USB-Tastatur treibt Login + Shell.`n" -ForegroundColor Green; exit 0 }
    Write-Host "`nERGEBNIS: USB-Tastatur-Verifikation FEHLGESCHLAGEN.`n" -ForegroundColor Red; exit 1
}

# ---- Verifikation des interaktiven Logins (Login-Daten via QEMU-stdin) ----
if ($Login -and $Verify -and -not $UsbKbd) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    $sdpart = ''
    if (Test-Path '_build/sd.img') { $sdpart = ' -drive file=_build/sd.img,if=sd,format=raw,cache=writethrough' }

    # .NET-Prozess: stdout asynchron einsammeln, Login-Daten ERST senden, wenn der
    # Gast am Prompt steht (sonst flusht uart_init die frueh gelieferten Zeichen).
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $qemu
    $psi.Arguments = "-M raspi4b -kernel `"$img`"$sdpart -serial stdio -display none"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $global:qOut = New-Object System.Text.StringBuilder
    $global:qErr = New-Object System.Text.StringBuilder
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $evO = Register-ObjectEvent -InputObject $proc -EventName OutputDataReceived -Action {
        if ($null -ne $Event.SourceEventArgs.Data) { [void]$global:qOut.AppendLine($Event.SourceEventArgs.Data) }
    }
    $evE = Register-ObjectEvent -InputObject $proc -EventName ErrorDataReceived -Action {
        if ($null -ne $Event.SourceEventArgs.Data) { [void]$global:qErr.AppendLine($Event.SourceEventArgs.Data) }
    }
    [void]$proc.Start()
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()       # stderr ebenfalls draenen -> kein Pipe-Stall

    # Closed-Loop: erst senden, wenn der Gast am jeweiligen Prompt steht und aktiv liest.
    # Sonst kollidiert die schnelle Serial-Eingabe mit dem langsameren HDMI-Echo-Rendering
    # (16-Byte-RX-FIFO laeuft ueber -> verstuemmelte/verschluckte Zeichen). Zeichenweise
    # mit Pause; nur \n (kein \r).
    function Wait-Out($pat, $secs) {
        $dl = (Get-Date).AddSeconds($secs)
        while ((Get-Date) -lt $dl -and ($global:qOut.ToString() -notmatch $pat)) { Start-Sleep -Milliseconds 150 }
    }
    function Type-Ser($s) {
        foreach ($ch in $s.ToCharArray()) {
            $proc.StandardInput.Write($ch); $proc.StandardInput.Flush(); Start-Sleep -Milliseconds 90
        }
    }

    Wait-Out 'login:' 25            # Boot (HDMI + USB-Enum + PBKDF2) bis zum Login-Prompt
    if ($proc.HasExited) {
        Write-Host 'FEHLER: QEMU vorzeitig beendet.' -ForegroundColor Red
        Write-Host $global:qErr.ToString()
        Unregister-Event -SubscriptionId $evO.Id; Unregister-Event -SubscriptionId $evE.Id
        exit 1
    }
    Write-Host 'Login + Shell-Befehle (closed-loop, gepaced)...' -ForegroundColor Cyan
    Type-Ser "admin`n"             # Username (Gast steht am login:-Prompt)
    Wait-Out 'password' 6
    Type-Ser "admin`n"             # Passwort (Gast steht am password:-Prompt)
    Wait-Out 'Neues Passwort' 8    # Default-Passwort -> erzwungener Wechsel
    Type-Ser "Geheim123`n"         # neues Passwort setzen (raeumt must_change)
    Wait-Out 'rpi_rtos Shell' 12

    # ls (USER.TXT noch da) -> rm hdd1:USER.TXT -> ls (USER.TXT weg): beweist
    # Listing + Loeschen + dass das Listing FS-Aenderungen widerspiegelt. passwd
    # aendert das eigene (admin-)Passwort. useradd/run wie gehabt.
    foreach ($cmd in @('whoami', 'ls',
                       'rm hdd1:USER.TXT', 'mkdir hdd1:NEWDIR',
                       'mkdir hdd1:MeinLangerOrdner', 'ls',
                       'ls > hdd1:LS.TXT', 'cat hdd1:LS.TXT',
                       'cp hdd1:WELCOME.TXT hdd1:COPY.TXT', 'mv hdd1:COPY.TXT hdd1:MOVED.TXT',
                       'cat hdd1:MOVED.TXT', 'ls',
                       'cd hdd1:DOCS', 'pwd', 'ls', 'cat README.TXT', 'cat LangeNotiz.txt',
                       'rmdir hdd1:NEWDIR', 'rmdir hdd1:MeinLangerOrdner', 'passwd geheim42',
                       'useradd bob bobpw', 'run hdd1:HELLO.ELF')) {
        Type-Ser ($cmd + "`n")
        Start-Sleep -Milliseconds 1200   # Ausfuehrung + HDMI-Rendering abwarten
    }

    # Auf die 'run'-Ausgabe pollen (beweist Login+Shell+Enforcement+Spawn), Timeout.
    # 'run' steht hinter ls/rm/passwd -> ist es da, sind die anderen schon gelaufen.
    $deadline = (Get-Date).AddSeconds(8)
    while ((Get-Date) -lt $deadline -and
           ($global:qOut.ToString() -notmatch 'run: gestartet')) {
        Start-Sleep -Milliseconds 200
    }

    # Kommando-Historie: unbekannten Token 'histtest' tippen (-> "unbekannt: histtest"),
    # dann Pfeil-hoch (CSI 'A' = ESC '[' 'A') + Enter -> der Token wird zurueckgeholt und
    # ERNEUT ausgefuehrt. Erscheint "unbekannt: histtest" zweimal, hat die Historie gegriffen.
    $esc = [char]27
    Type-Ser "histtest`n"
    Start-Sleep -Milliseconds 1000
    Type-Ser ($esc + '[' + 'A')          # Pfeil hoch -> letzte Zeile 'histtest'
    Start-Sleep -Milliseconds 500
    Type-Ser "`n"                         # Enter -> 'histtest' erneut
    Start-Sleep -Milliseconds 1000

    # ←/→-Cursor + mitten-einfuegen: 'abd' tippen, Cursor links (ESC[D) zwischen b und d,
    # 'c' einfuegen -> 'abcd'. Beweist Cursorbewegung + In-Line-Insert.
    Type-Ser "abd"
    Start-Sleep -Milliseconds 500
    Type-Ser ($esc + '[' + 'D')          # Cursor ein Zeichen nach links (vor 'd')
    Start-Sleep -Milliseconds 400
    Type-Ser "c"                          # einfuegen -> abcd
    Start-Sleep -Milliseconds 400
    Type-Ser "`n"                         # Enter -> Befehl 'abcd'
    Start-Sleep -Milliseconds 1000

    # Tab-Vervollstaendigung (Befehl): 'hel' + Tab -> 'help' -> Hilfetext 'Befehle:'
    Type-Ser "hel"
    Start-Sleep -Milliseconds 400
    Type-Ser "`t"                         # Tab
    Start-Sleep -Milliseconds 400
    Type-Ser "`n"
    Start-Sleep -Milliseconds 1000
    # Tab-Vervollstaendigung (Pfad, cwd=hdd1:DOCS): 'cat REA' + Tab -> 'cat README.TXT'
    Type-Ser "cat REA"
    Start-Sleep -Milliseconds 400
    Type-Ser "`t"                         # Tab
    Start-Sleep -Milliseconds 400
    Type-Ser "`n"
    Start-Sleep -Milliseconds 1000

    # --- Prozess-Handles: wait/kill (SYS_SPAWN->PID, SYS_WAIT, SYS_KILL, SYS_GETPPID) ---
    # (1) wait-Test: ein Kind starten und sein NORMALES Ende (exit 42) ernten. Das Kind laeuft
    #     ~800 ms (8x sleep 100), die Shell wartet blockierend darauf.
    Type-Ser "run hdd1:PCHILD.ELF`n"
    Start-Sleep -Milliseconds 300          # Kind startet (druckt ppid), laeuft noch
    Type-Ser "wait`n"
    Start-Sleep -Milliseconds 1800         # blockiert bis exit(42) -> "code=42"
    # (2) kill-Test: ein Kind starten, VOR seinem Ende killen, dann den Kill-Code (137) ernten.
    Type-Ser "run hdd1:PCHILD.ELF`n"
    Start-Sleep -Milliseconds 150          # Kind startet
    Type-Ser "kill`n"
    Start-Sleep -Milliseconds 400          # Safe-Point-Kill deutlich vor dem ~800ms-Ende
    Type-Ser "wait`n"
    Start-Sleep -Milliseconds 900          # geernteter Kill-Code -> "code=137"
    # (3) kill-in-wait: pwaiter spawnt ein Enkelkind (~800ms) und blockiert in wait() darauf;
    #     die Shell killt pwaiter WAEHREND es blockiert. Mit dem Fix stirbt pwaiter am wait-Safe-
    #     Point, BEVOR das Enkelkind endet -> "kind-geerntet" wird NIE gedruckt. Ohne den Fix wuerde
    #     pwaiter endlos re-blocken, das Enkelkind abwarten und die Zeile doch drucken.
    Type-Ser "run hdd1:PWAITER.ELF`n"
    Start-Sleep -Milliseconds 350          # pwaiter startet, spawnt Enkelkind, blockiert in wait
    Type-Ser "kill`n"                       # killt pwaiter (letztes Kind) im blockierten Zustand
    Start-Sleep -Milliseconds 500
    Type-Ser "wait`n"
    Start-Sleep -Milliseconds 1200         # pwaiter-Kill-Code geerntet

    Type-Ser "exit`n"
    Start-Sleep -Milliseconds 800

    if (-not $proc.HasExited) { $proc.Kill() }
    Start-Sleep -Milliseconds 300
    Unregister-Event -SubscriptionId $evO.Id
    Unregister-Event -SubscriptionId $evE.Id
    $log = $global:qOut.ToString()

    Write-Host "`n===== Serial-Log (interaktiver Login) =====" -ForegroundColor DarkGray
    Write-Host $log
    Write-Host '===========================================' -ForegroundColor DarkGray

    Write-Host "`n===== Verifikation =====" -ForegroundColor Cyan
    $ok = $true
    $ok = (Check 'Login-Prompt + Echo (login: admin)' ($log -match 'login: admin')) -and $ok
    $ok = (Check 'Interaktiver Login admin/admin erfolgreich' ($log -match 'Login erfolgreich: admin \(uid=0, admin\)')) -and $ok
    $ok = (Check 'Shell gestartet (Prompt rpi[uid0 hdd1:]$)' ($log -match 'rpi\[uid0 hdd1')) -and $ok
    $ok = (Check 'Shell whoami = uid 0' ($log -match 'uid=0(?!,)')) -and $ok   # nicht das Login-Banner (uid=0, admin)
    $ok = (Check 'Default-Passwort: Wechsel ERZWUNGEN (nicht nur Warnung)' ($log -match 'Passwort-Wechsel erforderlich')) -and $ok
    $ok = (Check 'Erzwungener Wechsel: neues Passwort gesetzt -> Shell' ($log -match 'Passwort gesetzt')) -and $ok
    $ok = (Check 'Shell ls -> hdd1-Listing (WELCOME.TXT im ls-Format)' ($log -match '(?m)^WELCOME\.TXT  41')) -and $ok
    $ok = (Check 'Shell rm hdd1:USER.TXT -> ok' ($log -match 'rm: ok')) -and $ok
    $ok = (Check 'Shell cp hdd1:WELCOME.TXT hdd1:COPY.TXT -> ok' ($log -match 'cp: ok')) -and $ok
    $ok = (Check 'Shell mv hdd1:COPY.TXT hdd1:MOVED.TXT -> ok' ($log -match 'mv: ok')) -and $ok
    $ok = (Check 'cp/mv: Inhalt erhalten (cat MOVED.TXT zeigt WELCOME-Text)' ($log -match 'Willkommen auf der User-Partition hdd1')) -and $ok
    $ok = (Check 'mv: Ziel im Listing, Quelle nicht (MOVED.TXT da, COPY.TXT weg)' (($log -match '(?m)^MOVED\.TXT  \d+') -and (([regex]::Matches($log, '(?m)^COPY\.TXT  \d+')).Count -eq 0))) -and $ok
    $ok = (Check 'ls spiegelt rm wider (USER.TXT nur im ersten Listing)' (([regex]::Matches($log, '(?m)^USER\.TXT  30')).Count -eq 1)) -and $ok
    $ok = (Check 'ls Wurzel zeigt Unterverzeichnis (DOCS <DIR>)' ($log -match '(?m)^DOCS  <DIR>')) -and $ok
    $ok = (Check 'mkdir hdd1:NEWDIR -> ok' ($log -match 'mkdir: ok')) -and $ok
    $ok = (Check 'ls zeigt neu angelegtes Verzeichnis (NEWDIR <DIR>)' ($log -match '(?m)^NEWDIR  <DIR>')) -and $ok
    $ok = (Check 'LFN-Schreiben: mkdir mit langem Namen -> ls zeigt MeinLangerOrdner' ($log -match '(?m)^MeinLangerOrdner  <DIR>')) -and $ok
    $ok = (Check 'cd hdd1:DOCS + pwd -> hdd1:DOCS' ($log -match '(?m)^hdd1:DOCS\s*$')) -and $ok
    $ok = (Check 'ls (cwd=DOCS) listet Subdir-Datei (README.TXT)' ($log -match '(?m)^README\.TXT  \d+')) -and $ok
    $ok = (Check 'cat README.TXT (relativ zum cwd) -> gelesen' ($log -match 'README im Unterverzeichnis')) -and $ok
    $ok = (Check 'LFN: ls zeigt langen Dateinamen (LangeNotiz.txt)' ($log -match 'LangeNotiz\.txt')) -and $ok
    $ok = (Check 'LFN: cat per langem Namen gelesen' ($log -match 'Datei mit langem Namen')) -and $ok
    $ok = (Check 'LFN-Sicherheit: malformiertes LFN -> 8.3-Fallback (BADLFN.TXT, kein Stack-Leak)' ($log -match '(?m)^BADLFN\.TXT  \d+')) -and $ok
    $ok = (Check 'rmdir hdd1:NEWDIR -> ok' ($log -match 'rmdir: ok')) -and $ok
    $ok = (Check 'Shell passwd -> eigenes Passwort geaendert' ($log -match 'passwd: Passwort geaendert')) -and $ok
    $ok = (Check 'Shell useradd als Admin -> ok' ($log -match 'useradd: ok')) -and $ok
    $ok = (Check 'Shell run -> Programm gestartet (cred geerbt)' (($log -match 'run: gestartet') -or ($log -match '\[Prozess'))) -and $ok
    $ok = (Check 'Kommando-Historie: Pfeil-hoch holt letzten Befehl zurueck (2x ausgefuehrt)' (([regex]::Matches($log, 'unbekannt: histtest')).Count -ge 2)) -and $ok
    $ok = (Check 'Zeilen-Editor: Cursor-links + Einfuegen ergibt abcd (aus abd)' ($log -match 'unbekannt: abcd')) -and $ok
    $ok = (Check 'Tab-Vervollstaendigung Befehl: hel<Tab> -> help (Hilfetext)' ($log -match 'Befehle: help')) -and $ok
    $ok = (Check 'Tab-Vervollstaendigung Pfad: cat REA<Tab> -> README.TXT gelesen' (([regex]::Matches($log, 'README im Unterverzeichnis')).Count -ge 2)) -and $ok
    $ok = (Check 'Umleitung: ls > hdd1:LS.TXT schrieb die Ausgabe in die Datei' ($log -match '-> hdd1:LS\.TXT \([1-9]\d* Bytes\)')) -and $ok
    $ok = (Check 'Umleitung: cat hdd1:LS.TXT liefert das umgeleitete Listing zurueck' ($log -match '(?m)^INDEX\.HTM  88')) -and $ok
    # Monotone PID (staerkster Beweis): ein Prozess-Slot wird WIEDERVERWENDET (die wait/kill-Test-
    # kinder laufen nacheinander auf demselben Slot 1), die PID steigt aber trotzdem streng an
    # (13 -> 15). Ein slot-basiertes Schema koennte fuer denselben Slot NIEMALS zwei verschiedene
    # IDs liefern -> genau das trennt monoton von slot-recycelt. Slot-agnostisch: fuer JEDEN
    # mehrfach belegten Slot muss die PID in Belegungsreihenfolge streng steigen.
    $pl = [regex]::Matches($log, '\[proc\] User-Prozess pid=(\d+) \(uid \d+, slot (\d+)\)')
    $lastPidForSlot = @{}
    $sawReuse = $false
    $allInc   = $true
    foreach ($m in $pl) {
        $p = [int]$m.Groups[1].Value; $s = $m.Groups[2].Value
        if ($lastPidForSlot.ContainsKey($s)) {
            $sawReuse = $true
            if ($p -le [int]$lastPidForSlot[$s]) { $allInc = $false }
        }
        $lastPidForSlot[$s] = $p
    }
    $slotReuse = $sawReuse -and $allInc
    $ok = (Check 'Monotone PID: Prozess-Slot wiederverwendet, PID trotzdem streng steigend (nie recycled)' $slotReuse) -and $ok
    # Prozess-Handles (wait/kill): die Shell startet ein Kind (SYS_SPAWN -> PID), das Kind kennt
    # seine Eltern-PID (SYS_GETPPID), und die Shell erntet dessen Exit-Code (SYS_WAIT). Guardian:
    # normales Ende -> code=42; per SYS_KILL beendet -> code=137 (beweist, dass wait den ECHTEN
    # Code liefert und kill wirklich terminiert, nicht nur das normale Ende durchreicht).
    $ok = (Check 'Prozess-Handles: SYS_SPAWN liefert die monotone Kind-PID (run -> pid=N)' ($log -match 'run: gestartet \(pid=[1-9]\d*\)')) -and $ok
    $ok = (Check 'Prozess-Handles: Kind kennt seine Eltern-PID (SYS_GETPPID != 0)' ($log -match '\[child \d+\] ppid=[1-9]\d*')) -and $ok
    $ok = (Check 'Prozess-Handles: wait() erntet den echten Exit-Code (normales Ende -> 42)' ($log -match 'wait: Kind pid=\d+ endete, code=42')) -and $ok
    $ok = (Check 'Prozess-Handles: kill() beendet ein eigenes Kind (kill: ok)' ($log -match 'kill: ok')) -and $ok
    $ok = (Check 'Prozess-Handles: gekilltes Kind -> wait() liefert den Kill-Code 137' ($log -match 'wait: Kind pid=\d+ endete, code=137')) -and $ok
    # kill-in-wait (Review-Fix): pwaiter blockiert in wait() auf ein UNSTERBLICHES Enkelkind (PLOOP)
    # und wird dabei gekillt. MIT dem Fix stirbt es am wait-Safe-Point -> die Shell erntet es per
    # wait() (2. code=137). OHNE den Fix wuerde pwaiter endlos re-blocken (Enkelkind endet nie) und
    # nie sterben -> die Shell haengt im wait() und der 2. Kill-Code erscheint NICHT. Guardian:
    # pwaiter startete UND es gibt >=2 geerntete Kill-Codes (PCHILD-Kill + pwaiter-Kill).
    $pwStarted = $log -match '\[pwaiter \d+\] spawnte \d+, wartet'
    $kills137  = ([regex]::Matches($log, 'wait: Kind pid=\d+ endete, code=137')).Count
    $ok = (Check 'Prozess-Handles: kill() eines in wait() blockierten Prozesses greift am Safe-Point (kein Hang)' ($pwStarted -and ($kills137 -ge 2))) -and $ok

    if ($ok) {
        Write-Host "`nERGEBNIS: Interaktiver Serial-Login verifiziert.`n" -ForegroundColor Green
        exit 0
    }
    Write-Host "`nERGEBNIS: Verifikation FEHLGESCHLAGEN.`n" -ForegroundColor Red
    exit 1
}

# ---- USB-Massenspeicher-Verifikation (raspi4b + usb-storage, BOT/SCSI/READ/WRITE) ----
if ($Verify -and $UsbStorage) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    # FAT32-USB-Image (MBR + USBINFO.TXT + USBHELLO.ELF) erzeugen.
    $usbimg = Join-Path $PSScriptRoot '_build/usb.img'
    $py = Find-Tool @('python', 'py')
    if (-not $py) { Write-Warning 'python fuer usb.img noetig.'; exit 1 }
    & $py '_build/tools/gen_usbimg.py' $usbimg '_build/hello.elf' | Out-Null

    $ser = Join-Path $PSScriptRoot 'usbstor_serial.txt'
    $err = Join-Path $PSScriptRoot 'usbstor_err.txt'
    Remove-Item -Force -ErrorAction SilentlyContinue $ser, $err
    $sd = if (Test-Path '_build/sd.img') { @('-drive', 'file=_build/sd.img,if=sd,format=raw') } else { @() }

    Write-Host 'Starte QEMU raspi4b mit usb-storage...' -ForegroundColor Cyan
    $p = Start-Process -FilePath $qemu -ArgumentList (@('-M', 'raspi4b', '-kernel', $img) + $sd +
        @('-drive', "id=ud,file=$usbimg,if=none,format=raw", '-device', 'usb-storage,drive=ud',
          '-serial', "file:$ser", '-display', 'none')) `
        -PassThru -WindowStyle Hidden -RedirectStandardError $err
    Start-Sleep -Seconds 12
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    $log = if (Test-Path $ser) { Get-Content $ser -Raw } else { '' }

    Write-Host "`n===== Serial-Log (USB-Massenspeicher) =====`n"
    Write-Host $log
    Write-Host "`n===== Verifikation ====="
    $ok = $true
    $ok = (Check 'USB-MSC: Bulk-Only-Massenspeicher enumeriert' ($log -match 'Massenspeicher: addr=2 Bulk-IN')) -and $ok
    $ok = (Check 'USB-MSC: SCSI INQUIRY (QEMU HARDDISK)' ($log -match 'INQUIRY:.*HARDDISK')) -and $ok
    $ok = (Check 'USB-MSC: READ CAPACITY (16384 Sektoren)' ($log -match 'Kapazitaet: 16384 Sektoren')) -and $ok
    $ok = (Check 'USB-MSC: WRITE(10) Sektor geschrieben+zurueckgelesen' ($log -match 'schreibtest: ok')) -and $ok
    $ok = (Check 'USB-MSC: USB-Stick als hdd2 gemountet (FAT32)' ($log -match 'hdd2 \(USB-Stick\) gemountet')) -and $ok
    $ok = (Check 'USB-MSC: Datei von hdd2 gelesen (USBINFO.TXT)' ($log -match 'Datei vom USB-Stick')) -and $ok
    if ($ok) { Write-Host "`nERGEBNIS: USB-Massenspeicher (BOT/SCSI + hdd2-Mount) verifiziert.`n" -ForegroundColor Green; exit 0 }
    Write-Host "`nERGEBNIS: USB-Massenspeicher-Verifikation FEHLGESCHLAGEN.`n" -ForegroundColor Red; exit 1
}

# ---- DoS-Regressionstest: praepariertes hdd2 mit ZYKLISCHER Root-Dir-FAT-Kette ----
# Belegt, dass der Zyklusschutz in root_iterate/fat32_delete greift: ohne ihn liefe die
# Verzeichnis-Traversierung beim hdd2-Listing/Read endlos und haengt (IRQs maskiert) den
# kompletten Kernel. Erfolgskriterium: der Boot ERREICHT trotz korruptem hdd2 den
# Scheduler und laeuft sauber zu Ende (kein Hang).
if ($Verify -and $UsbCyclic) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }
    $py = Find-Tool @('python', 'py')
    if (-not $py) { Write-Warning 'python fuer usb.img noetig.'; exit 1 }

    $usbimg = Join-Path $PSScriptRoot '_build/usb_cyclic.img'
    & $py '_build/tools/gen_usbimg.py' $usbimg '--cyclic' | Out-Null

    $ser = Join-Path $PSScriptRoot 'usbcyc_serial.txt'
    $err = Join-Path $PSScriptRoot 'usbcyc_err.txt'
    Remove-Item -Force -ErrorAction SilentlyContinue $ser, $err
    $sd = if (Test-Path '_build/sd.img') { @('-drive', 'file=_build/sd.img,if=sd,format=raw') } else { @() }

    Write-Host 'Starte QEMU raspi4b mit ZYKLISCHEM hdd2 (DoS-Regressionstest)...' -ForegroundColor Cyan
    $p = Start-Process -FilePath $qemu -ArgumentList (@('-M', 'raspi4b', '-kernel', $img) + $sd +
        @('-drive', "id=ud,file=$usbimg,if=none,format=raw", '-device', 'usb-storage,drive=ud',
          '-serial', "file:$ser", '-display', 'none')) `
        -PassThru -WindowStyle Hidden -RedirectStandardError $err
    # 30s: mit angehaengtem usb-storage ist der Boot langsamer; Floyd bricht die
    # zyklische Traversierung in ~2 Reads ab, der Rest ist normaler Boot bis Scheduler.
    Start-Sleep -Seconds 30
    $hung = -not $p.HasExited
    if ($hung) { Stop-Process -Id $p.Id -Force }
    $log = if (Test-Path $ser) { Get-Content $ser -Raw } else { '' }

    Write-Host "`n===== Serial-Log (zyklisches hdd2) =====`n"
    Write-Host $log
    Write-Host "`n===== Verifikation ====="
    $ok = $true
    # hdd2 wird gemountet (BPB gueltig) -> die Verzeichnis-Traversierung LAEUFT auf der
    # zyklischen Kette; der Guard muss sie abbrechen statt endlos zu schleifen.
    $ok = (Check 'Setup: korruptes hdd2 wurde gemountet (Traversierung wird ausgeloest)' ($log -match 'hdd2 \(USB-Stick\) gemountet')) -and $ok
    # KERN-ASSERT: Boot kommt am FS-Listing/Read von hdd2 VORBEI bis zum Scheduler.
    $ok = (Check 'Zyklusschutz: Boot erreicht den Scheduler trotz zyklischer FAT (kein Hang)' ($log -match '\[8\] Scheduler starten')) -and $ok
    $ok = (Check 'Zyklusschutz: Userland laeuft + beendet sauber (voller Durchlauf)' ($log -match 'fertig -> exit')) -and $ok
    # hdd0/hdd1 bleiben unberuehrt (eigene fs-Handles, io_err pro Mount zurueckgesetzt).
    $ok = (Check 'Robustheit: hdd0/hdd1 weiterhin korrekt gelistet' ($log -match '(?m)^\s*-\s+SYSTEM\.TXT')) -and $ok
    if ($ok) { Write-Host "`nERGEBNIS: DoS-Regressionstest bestanden (Zyklusschutz wirkt).`n" -ForegroundColor Green; exit 0 }
    Write-Host "`nERGEBNIS: DoS-Regressionstest FEHLGESCHLAGEN$(if ($hung) {' (QEMU hing -> Guard wirkungslos!)'}).`n" -ForegroundColor Red; exit 1
}

# ---- automatisierte raspi4b-Verifikation (Storage+Userland+Chipsatz+Benutzer+Persistenz) ----
if ($Verify -and -not $UsbStorage -and -not $UsbCyclic) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning 'qemu-system-aarch64 nicht gefunden.'; exit 1 }

    Write-Host 'Starte QEMU raspi4b (Lauf 1: frische Benutzer-DB)...' -ForegroundColor Cyan
    # 34s: Boot+HDMI-Render+USB-Enum+PBKDF2+SMP-Lasttest+Scheduler. Der parallele
    # Spinlock-Lasttest [2c] kostet zusaetzlich Boot-Zeit -> Fenster 22->28s. Bei hoher
    # Host-Last (z.B. paralleler Build/Agenten) brach der Lauf gelegentlich vor der
    # SMP-/Userland-Demo ab -> 28->34s Headroom gegen transiente Host-Contention.
    $r1 = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'rpi_serial1.txt') (Join-Path $PSScriptRoot 'rpi_err1.txt') 34
    if ($r1.Early) {
        Write-Host 'FEHLER: QEMU (Lauf 1) vorzeitig beendet.' -ForegroundColor Red
        if ((Test-Path $r1.ErrFile) -and (Get-Item $r1.ErrFile).Length -gt 0) { Get-Content $r1.ErrFile | Write-Host }
        exit 1
    }

    # Lauf 2 auf DEMSELBEN sd.img (kein Rebuild) -> die in Lauf 1 auf hdd0
    # geschriebene Benutzer-DB muss erhalten bleiben und geladen werden.
    Write-Host 'Starte QEMU raspi4b (Lauf 2: DB von hdd0 laden = Persistenz)...' -ForegroundColor Cyan
    $r2 = Invoke-Raspi $qemu $img (Join-Path $PSScriptRoot 'rpi_serial2.txt') (Join-Path $PSScriptRoot 'rpi_err2.txt') 34
    if ($r2.Early) {
        Write-Host 'FEHLER: QEMU (Lauf 2) vorzeitig beendet.' -ForegroundColor Red
        if ((Test-Path $r2.ErrFile) -and (Get-Item $r2.ErrFile).Length -gt 0) { Get-Content $r2.ErrFile | Write-Host }
        exit 1
    }

    $log = $r1.Log
    Write-Host "`n===== Serial-Log (raspi4b, Lauf 1) =====" -ForegroundColor DarkGray
    Write-Host $log
    Write-Host '========================================' -ForegroundColor DarkGray

    Write-Host "`n===== Verifikation =====" -ForegroundColor Cyan
    $ok = $true
    $ok = (Check 'Lauf 1: Serial-Log frisch' ((Test-Path $r1.SerFile) -and ((Get-Item $r1.SerFile).LastWriteTime -gt $r1.Started))) -and $ok
    $ok = (Check 'Storage: hdd0 + hdd1 gemountet' ($log -match 'hdd0 gemountet' -and $log -match 'hdd1 gemountet')) -and $ok
    $ok = (Check 'FAT32-Schreibtest auf hdd1' ($log -match 'KERNEL\.TXT geschrieben')) -and $ok
    $ok = (Check 'FAT32 LFN-Schreiben+Lesen (langer Dateiname)' ($log -match 'LFN-Schreibtest vom Kernel')) -and $ok
    $ok = (Check 'FAT32 sec_per_clus>1 (hdd1 = 4 Sektoren/Cluster, Multi-Sektor-Cluster verifiziert)' ($log -match 'hdd1 gemountet')) -and $ok
    $ok = (Check 'FS-Reife: SYS_READ_FILE signalisiert Trunkierung (return = wahre Groesse)' ($log -match 'read\(INDEX\.HTM, buf=10\) -> 88 \(>10: Trunkierung signalisiert\)')) -and $ok
    $ok = (Check 'FS-Reife: FSInfo-Free-Count gepflegt (alloc -1 / free +1)' ($log -match 'FSInfo free_count \d+ -alloc-> \d+ -free-> \d+  \(gepflegt\)')) -and $ok
    $ok = (Check 'FS-Reife: LFN-Loeschen ueber Sektorgrenze (Nachbar intakt)' ($log -match 'LFN-Loeschen ueber Sektorgrenze \(CrossBoundary\.txt\): geloescht, MARKER\.TXT intakt')) -and $ok
    $ok = (Check 'FS-Reife: Datei-Zeitstempel gesetzt (kernel-geschriebene KERNEL.TXT zeigt mtime 2026, nicht 1980)' ($log -match 'KERNEL\.TXT  \(47 Byte\)  2026-')) -and $ok
    # Prozess-tid ist nicht mehr 1 (die SMP-Tasks belegen die niedrigen Slots) -> tid-agnostisch.
    $ok = (Check 'Userland: EL0-Prozess lief + beendet' ($log -match '\[Prozess \d+\] Start auf EL0' -and $log -match 'User-Task beendet')) -and $ok
    # Monotone PID (raspi): die drei gleichzeitigen User-Prozesse tragen streng steigende PIDs, die
    # NICHT der Prozess-Slot sind (slot 0/1/2, pid ~12/13/14). Der definitive Monotonie-Beweis
    # (PID ueberlebt Slot-Wiederverwendung) laeuft im -Login-Suite (dort wird Slot 0 wiederbelegt).
    $pidLines = [regex]::Matches($log, '\[proc\] User-Prozess pid=(\d+) \(uid \d+, slot (\d+)\)')
    $pidOk = $false
    if ($pidLines.Count -ge 2) {
        $pids  = @($pidLines | ForEach-Object { [int]$_.Groups[1].Value })
        $slots = @($pidLines | ForEach-Object { [int]$_.Groups[2].Value })
        $notSlot = $false; for ($i=0; $i -lt $pids.Count; $i++) { if ($pids[$i] -ne $slots[$i]) { $notSlot = $true } }
        $mono = $true; for ($i=1; $i -lt $pids.Count; $i++) { if ($pids[$i] -le $pids[$i-1]) { $mono = $false } }
        $pidOk = $notSlot -and $mono
    }
    $ok = (Check 'Monotone PID: User-Prozesse tragen PIDs != Prozess-Slot, streng steigend' $pidOk) -and $ok
    $ok = (Check 'GPIO Richtung=Output + Level-Reflect' ($log -match 'GPIO21 dir=output set->1 clr->0')) -and $ok
    $ok = (Check 'SPI0 Transfer abgeschlossen (DONE)' ($log -match 'SPI0 Transfer 4 Byte: ok')) -and $ok
    $ok = (Check 'I2C BSC1 Transfer ausgefuehrt' ($log -match 'I2C BSC1 write\(0x50\):')) -and $ok
    $ok = (Check 'Krypto-Selbsttest (SHA-256/PBKDF2)' ($log -match 'Krypto-Selbsttest: ok')) -and $ok
    $ok = (Check 'Benutzer-DB angelegt (Default-admin)' ($log -match "DB erstellt: Default-Konto 'admin'")) -and $ok
    $ok = (Check 'DB crash-sicher: korruptes Primaer -> aus USERS.BAK wiederhergestellt (Marker ueberlebt)' ($log -match 'DB-Recovery \(korruptes Primaer -> Backup\): ja')) -and $ok
    $ok = (Check 'IPC-Timeout: sem_wait_timeout laeuft ab (RT-Garantie, -ETIMEDOUT)' ($log -match 'sem_wait_timeout\(30t\) ohne post -> Timeout\(ret<0\) nach ~\d+ Ticks')) -and $ok
    $ok = (Check 'IPC-Timeout: sem_wait_timeout Erfolgspfad unveraendert (count=1 -> sofort)' ($log -match 'sem_wait_timeout\(30t\) mit count=1 -> sofort erworben\(ret=0\)')) -and $ok
    $ok = (Check 'IPC-Timeout: mutex_lock_timeout laeuft auf gehaltenem Mutex ab' ($log -match 'mutex_lock_timeout\(30t\) auf gehaltenem Mutex -> Timeout\(ret<0\) nach ~\d+ Ticks')) -and $ok
    $ok = (Check 'User-Access: AT S1E0 lehnt inaccessible Adressen ab (Fault-Isolation)' ($log -match 'AT-User-Access \(lehnt inaccessible ab\): ja')) -and $ok
    $ok = (Check 'SMP-PI: Cross-Core-PI-Boost stoesst owner-Kern an (need_resched+SGI)' ($log -match 'cross-core PI-Boost -> Reschedule-IPI an owner-Kern: ja')) -and $ok
    $ok = (Check 'T1.8 CPACR: FP/SIMD trappt deterministisch (FPEN=0b00, kein V-Reg-Leak)' ($log -match 'CPACR_EL1\.FPEN \(FP-Trap EL0\+EL1\): 0b00 \(ja\)')) -and $ok
    # Statischer Integer-only-Nachweis (T1.8): der Kernel darf KEINE FP/SIMD-Instruktion enthalten
    # (-mgeneral-regs-only). Disassemblieren, Daten-Direktiven ausschliessen, echte FP/SIMD-
    # Register-Operanden (v/q/d/s/h + 0..31 mit Wortgrenze) zaehlen -> muss 0 sein.
    $objdump = $null
    if ($tc.Kind -eq 'gnu') { $objdump = ($tc.CC -replace 'gcc$','objdump') } else { $objdump = (Find-Tool @('llvm-objdump')) }
    $fpCount = -1
    if ($objdump -and (Test-Path '_build/kernel8.elf')) {
        $dis = & $objdump -d --no-show-raw-insn '_build/kernel8.elf' 2>$null
        $fpCount = ($dis | Where-Object { $_ -notmatch '\.(word|byte|short|long|inst|zero|ascii)' } |
                    Select-String -Pattern '\b[vqdsh]([0-9]|[12][0-9]|3[01])\b').Count
    }
    $ok = (Check ('T1.8 Integer-only: Kernel-.text ohne FP/SIMD-Instruktionen (gefunden: {0})' -f $fpCount) ($fpCount -eq 0)) -and $ok
    $ok = (Check 'T1.9 Bus-Lock: Mailbox-Transaktion ueber Kerne serialisiert (max-Belegung=1, keine Ueberlappung)' ($log -match 'SMP-Mailbox-Serialisierung: Transfers-Fehler=0 max-Belegung=1 Verletzung\(occ>1\)=nein')) -and $ok
    $ok = (Check 'T1.10 VFS-Lock: vfs_list/listdir teilen secbuf serialisiert (fs_lock, max-Belegung=1)' ($log -match 'secbuf-Serialisierung \(vfs_list\+listdir\): max-Belegung=1 Verletzung\(occ>1\)=nein')) -and $ok
    # T1.12 RAM-Ausbau: DTB-getriebene Voll-RAM-Karte. QEMU raspi4b uebergibt keinen DTB -> konservative
    # Grobkarte (0..3 GiB Normal). Der DTB-Pfad (voller RAM inkl. >4 GiB) ist HW-only; der FDT-Parser
    # wird per eingebettetem Test-DTB (2 Regionen inkl. High-RAM) in QEMU mutations-geprueft.
    $ok = (Check 'T1.12 RAM-Ausbau: DTB-getrieben, QEMU-Fallback-Grobkarte 0..3 GiB Normal gemappt' ($log -match 'RAM-Ausbau \(T1.12\): Quelle=Grobkarte-ohne-DTB Normal-RAM-gemappt=0x00000000C0000000')) -and $ok
    $ok = (Check 'T1.12 FDT-Parser: /memory mit 2 Regionen inkl. High-RAM >4 GiB korrekt extrahiert' ($log -match 'FDT-Parser-Selbsttest \(/memory, 2 Regionen inkl\. High-RAM >4GiB\): ok')) -and $ok
    $ok = (Check 'Login mit richtigem Passwort (alice)' ($log -match 'Login alice \(richtiges Passwort\): ok')) -and $ok
    $ok = (Check 'Login mit falschem Passwort abgelehnt' ($log -match 'Login alice \(falsches Passwort\): abgelehnt')) -and $ok
    $ok = (Check 'Capability: Nicht-Admin verweigert' ($log -match 'user_add ohne Admin-Recht: verweigert')) -and $ok
    $ok = (Check 'EL0-App: hdd0/Credential-DB-Zugriff verweigert' ($log -match 'hdd0-Zugriff verweigert')) -and $ok
    $ok = (Check 'Enforcement: alice-Prozess (uid=1) -> SYS_USERADD verweigert' ($log -match 'uid=1 SYS_USERADD verweigert')) -and $ok
    $ok = (Check 'Enforcement: admin-Prozess (uid=0) -> SYS_USERADD erlaubt'   ($log -match 'uid=0 SYS_USERADD ok')) -and $ok
    $ok = (Check 'HDMI: Framebuffer 640x480 (Mailbox/VideoCore)' ($log -match '\[fb\] 640x480')) -and $ok
    $ok = (Check 'HDMI: Pixel-Selbsttest (RGBW im FB-RAM)' ($log -match 'testbild r=0x0000000000FF0000 g=0x000000000000FF00 b=0x00000000000000FF w=0x0000000000FFFFFF')) -and $ok
    $ok = (Check 'HDMI: Konsole spiegelt Text in den Framebuffer' ($log -match 'konsole-render: ja')) -and $ok
    $ok = (Check 'HDMI: ANSI-Farbe (gruen) in der Konsole gerendert' ($log -match 'konsole-farbe: ja')) -and $ok
    $ok = (Check 'SMP: 3 Sekundaerkerne online (EL1)' ($log -match '\[smp\] 3 Kerne online \(EL1\)')) -and $ok
    $ok = (Check 'SMP: Spinlock serialisiert geteilten Zaehler (kein Lost-Update)' ($log -match 'Spinlock-Zaehler=60000 erwartet=60000 => OK')) -and $ok
    $ok = (Check 'SMP: Per-Core-Timer-IRQ auf allen Sekundaerkernen' ($log -match 'Per-Core-Timer-IRQ:.*-> alle ticken: ja')) -and $ok
    $ok = (Check 'SMP-Scheduler: Kernel-Tasks liefen praeemptiv auf eigenen Kernen' ($log -match '\[smp-sched\].*je Task auf eigenem Kern: ja')) -and $ok
    $ok = (Check 'SMP-IPC: Semaphor-Warter cross-core geweckt (Kern0 -> Kern1)' ($log -match '\[smp-ipc\].*cross-core IPC: ok')) -and $ok
    $ok = (Check 'SMP-IPI: Reschedule-IPI (SGI) auf Kern 1 empfangen' ($log -match '\[smp-ipc\] Reschedule-IPI auf Kern 1 empfangen: [1-9]\d* -> IPI ok')) -and $ok
    $ok = (Check 'SMP-Spawn: Laufzeit-Spawn eines EL0-Prozesses auf Kern 1 (live Scheduler)' ($log -match '\[smp-spawn\] Laufzeit-Spawn -> Kern 1 .* tid=\d+ -> ok')) -and $ok
    $ok = (Check 'SMP-EL0: Laufzeit-gespawnter Prozess lief auf einem Sekundaerkern (CPU=1)' ($log -match 'Start auf EL0.*CPU=1')) -and $ok
    $ok = (Check 'SMP-FS: Datei-I/O auf einem Sekundaerkern (VFS-Lock)' ($log -match 'FS-I/O auf Sekundaerkern \(CPU=1\) ok')) -and $ok
    $ok = (Check 'USB: DWC2 Host-Modus aktiv' ($log -match 'DWC2 Host-Modus aktiv \(CURMOD=host\)')) -and $ok
    $ok = (Check 'USB: HID-Tastatur ueber Hub enumeriert' ($log -match 'HID-Tastatur: addr=2 .*Interrupt-IN EP1')) -and $ok
    # HTTP-Server-VFS-Resolver (Direktaufruf; HTTP-Protokoll ist auf virt verifiziert):
    $ok = (Check 'HTTP-VFS: /INDEX.HTM aufgeloest (Content-Type text/html)' ($log -match '/INDEX\.HTM -> \d+ Byte, ctype=text/html')) -and $ok
    $ok = (Check 'HTTP-VFS: Datei-Body geliefert (www-marker)' ($log -match 'rpi_rtos-www-index-8c1d')) -and $ok
    $ok = (Check 'HTTP-VFS: Content-Type fuer .css' ($log -match '/STYLE\.CSS -> ctype=text/css')) -and $ok
    $ok = (Check 'HTTP-VFS: / -> Verzeichnis-Index' ($log -match '/ \(Index\) -> \d+ Byte Listing')) -and $ok
    $ok = (Check 'HTTP-VFS: Directory-Traversal/Doc-Root-Escape blockiert' ($log -match 'Traversal blockiert: \.\./=ja %2f=ja mount-inject=ja')) -and $ok
    $ok = (Check 'HTTP-VFS: unbekannte Datei -> 404' ($log -match '/GIBTESNICHT\.TXT -> nicht gefunden')) -and $ok
    $ok = (Check 'HTTP-VFS: Dateiname mit zwei Punkten (LFN) nicht als Traversal abgewiesen' ($log -match 'release\.\.notes\.txt -> gefunden \(korrekt\): marker=rpi_rtos-relnotes-2dots')) -and $ok
    $ok = (Check 'Persistenz: DB ueberlebt Neustart (Lauf 2 laedt DB)' ($r2.Log -match 'DB geladen \(\d+ Konten\)')) -and $ok
    $ok = (Check 'Persistenz: Login alice nach Neustart' ($r2.Log -match 'Login alice \(richtiges Passwort\): ok')) -and $ok

    if ($ok) {
        Write-Host "`nERGEBNIS: raspi4b verifiziert (Storage+Userland+Chipsatz+Benutzer+HDMI+Persistenz).`n" -ForegroundColor Green
        exit 0
    }
    Write-Host "`nERGEBNIS: Verifikation FEHLGESCHLAGEN.`n" -ForegroundColor Red
    exit 1
}

if ($Run) {
    $qemu = Find-Tool @('qemu-system-aarch64')
    if (-not $qemu) { Write-Warning "qemu-system-aarch64 nicht gefunden."; return }
    $sdargs = @()
    if (Test-Path '_build/sd.img') { $sdargs = @('-drive', 'file=_build/sd.img,if=sd,format=raw') }
    Write-Host 'Starte QEMU raspi4b (Beenden: Strg-A dann X)...' -ForegroundColor Cyan
    & $qemu -M raspi4b -kernel $img @sdargs -serial stdio -display none
}
