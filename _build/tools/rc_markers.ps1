# tools/rc_markers.ps1 -- Zentrale RC-Marker-Taxonomie (T1.17).
#
# EINE Quelle der Wahrheit fuer die Boot-Marker des RC-Produktions-Images (-Release):
#   - build.ps1        (QEMU-Verifikation via -serial file:)
#   - tools/hw_verify.ps1  (HW-/QEMU-Serial-Poller)
# So driften QEMU- und HW-Assertion nicht auseinander.
#
# Einbinden:  . (Join-Path $PSScriptRoot 'rc_markers.ps1')   bzw. relativ zum Aufrufer.

# Pflicht-Marker eines gesunden RC-Boots (Reihenfolge = Boot-Chronologie). Jeder Eintrag:
#   Name    = Anzeige im [PASS]/[FAIL]-Report
#   Pattern = Regex fuer -match gegen das gesammelte Serial-Log
$RcReadyMarkers = @(
    # Boot-Eintritt: unbedingter Banner ganz am Anfang von kmain() (VOR jeder Storage-Init) --
    # bestaetigt nur, dass der Kernel bis kmain kam. Storage-Gesundheit erzwingen erst die
    # spaeteren, storage_ok-gegateten Marker (DB + RC-READY); daher NICHT "Storage" nennen.
    [pscustomobject]@{ Name = 'Kernel-Banner (Boot-Eintritt in kmain)';    Pattern = 'rpi_rtos\s+-\s+Storage: hdd0 \+ hdd1' }
    [pscustomobject]@{ Name = 'Start auf EL1';                             Pattern = '(?m)^Start auf EL1' }
    [pscustomobject]@{ Name = 'Krypto-Selbsttest bestanden (fail-closed)'; Pattern = 'Krypto-Selbsttest: ok' }
    # T1.8-Haertung zur LAUFZEIT belegt (Review T3.x): das RC-Image traegt FPEN=0b00 -> FP/SIMD
    # trappt an EL0+EL1. GUI-/Vk-Builds (0b11) sind KEINE RC-Images und laufen nicht ueber diese Liste.
    [pscustomobject]@{ Name = 'FP/SIMD-Trap aktiv (CPACR FPEN=0b00, T1.8)'; Pattern = 'CPACR_EL1\.FPEN=0b00 \(FP/SIMD-Trap aktiv' }
    # Drei gesunde DB-Pfade: r==0 "DB erstellt", r==1 "DB geladen", r==2 aus Backup wiederhergestellt.
    [pscustomobject]@{ Name = 'Benutzer-DB initialisiert (fail-closed)';   Pattern = 'DB (erstellt|geladen)|aus Backup USERS\.BAK wiederhergestellt' }
    [pscustomobject]@{ Name = 'RC-READY (Boot bis Login-Bereitschaft)';    Pattern = 'RC-READY' }
    [pscustomobject]@{ Name = 'Interaktiver Login-Prompt';                 Pattern = '(?m)^login:' }
)

# Bereitschafts-Muster: sobald es (samt aller Pflicht-Marker) erscheint, ist der Boot durch.
# Der Poller kann darauf frueh gruen abbrechen (statt bis zum Timeout zu lesen).
$RcReadyPattern = 'RC-READY'

# Definitiver Fehlschlag: erscheint dieses Muster, ist der Boot gescheitert -> sofort abbrechen.
# Deckt den fail-closed-Halt (kpanic -> "KERNEL-HALT") UND einen echten Exception-/Stack-Panic
# ("*** KERNEL PANIC ...", exceptions.c/sched.c) ab, damit der Poller nicht bis zum Timeout laeuft.
$RcHaltPattern = 'KERNEL-HALT|KERNEL PANIC'

# GREP-CLEAN (T1.1): diese Nadeln duerfen im grep-clean Produktions-Image NICHT auftauchen
# (keine hartkodierten Credentials, kein Demo-/Selbsttest-Scaffolding).
$RcForbidden = @('alice', 'geheim123', 'mallory', 'GPIO21', 'SPI0 Transfer', 'I2C BSC1',
                 'schreibtest', 'Schreibtest (hdd1', '[5b]', '[5e]', '[5f]', 'Traversal', 'LangerDateiname')
