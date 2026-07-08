# tools/vision_markers.ps1 -- Marker-Satz fuer den -Vision-Flavor (KI-Bildauswertung) auf HW / QEMU.
#
# Von tools/hw_verify.ps1 via -MarkerSet vision dot-gesourct. -Vision traegt FPEN=0b11 (NEON) +
# Scaffolding -> eigener Marker-Satz, kein grep-clean. Fokus: Boot + die HW-relevanten Selbsttest-
# Stationen (4-Kern-Parallel-GEMM A1.5, end-to-end M0, Echtzeit-Schleife A5) + Abschluss.
$VisionReadyMarkers = @(
    [pscustomobject]@{ Name = 'Kernel-Banner (Boot-Eintritt)';         Pattern = 'rpi_rtos\s+-\s+Storage: hdd0 \+ hdd1' }
    [pscustomobject]@{ Name = 'Start auf EL1';                         Pattern = '(?m)^Start auf EL1' }
    [pscustomobject]@{ Name = 'CPACR FPEN=0b11 (EL0-FP/NEON frei)';    Pattern = 'CPACR_EL1\.FPEN=0b11' }
    [pscustomobject]@{ Name = 'UVC-Klassen-Layer ok (parse/probe/assemble)'; Pattern = '\[uvc\] class-layer: parse\(vs\+bulk-in\)=ok probe=ok assemble\(2pkt\+eof\)=ok' }
    [pscustomobject]@{ Name = 'AIVISION.ELF gestartet';               Pattern = '\[vision\] AIVISION\.ELF' }
    [pscustomobject]@{ Name = 'A1.1 NEON-fp32-sgemm ok';               Pattern = '\[aivision\] A1\.1 sgemm NEON-fp32' }
    [pscustomobject]@{ Name = 'A1.5 4-Kern-Parallel-GEMM ok';          Pattern = '\[aivision\] A1\.5 parallel-sgemm' }
    [pscustomobject]@{ Name = 'M0 end-to-end Klassifikation ok';       Pattern = '\[aivision\] M0 klassifikation' }
    [pscustomobject]@{ Name = 'A5 Echtzeit-Schleife (Grab+Loop) ok';   Pattern = '\[aivision\] A5 loop' }
    [pscustomobject]@{ Name = 'A5.2 FPS-Mess-Infrastruktur ok';        Pattern = '\[aivision\] A5\.2 fps-infra: ticks-monoton=ok' }
    [pscustomobject]@{ Name = 'Engine-Selbsttest fertig';             Pattern = '\[aivision\] engine-selftest fertig' }
    [pscustomobject]@{ Name = 'Scheduler gestartet (alle Kerne)';      Pattern = '\[8\] Scheduler starten' }
)
# Fail-fast NUR auf echte Halts/Panics ODER einen Vision-EIGENEN Fehlermarker -- NICHT auf
# harmloses "fehlgeschlagen" im Standard-Scaffolding (RTOS_SELFTEST-Boot).
$VisionHaltPattern = 'KERNEL-HALT|KERNEL PANIC|\[vision\] FEHLER|\[aivision\].*FEHLER|\[uvc\].*FEHLER'
$VisionForbidden = @()
