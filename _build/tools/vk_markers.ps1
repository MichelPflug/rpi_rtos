# tools/vk_markers.ps1 -- Marker-Satz fuer den -Vk-Flavor (Vulkan-Selbsttest) auf echter HW / QEMU.
#
# Von tools/hw_verify.ps1 via -MarkerSet vk dot-gesourct. Anders als rc_markers.ps1 (RC-Image,
# FPEN=0b00, grep-clean) traegt der -Vk-Boot FPEN=0b11 + Scaffolding (RTOS_SELFTEST) -> eigener
# Marker-Satz, KEIN grep-clean. Die Marker sind exakt die Serial-Ausgaben, die vktest/vkcube ohnehin
# drucken (dieselben, die build.ps1 im QEMU -Vk -Verify assertet) -- fokussiert auf die Boot-Chronologie.
$VkReadyMarkers = @(
    [pscustomobject]@{ Name = 'Kernel-Banner (Boot-Eintritt)';         Pattern = 'rpi_rtos\s+-\s+Storage: hdd0 \+ hdd1' }
    [pscustomobject]@{ Name = 'Start auf EL1';                         Pattern = '(?m)^Start auf EL1' }
    [pscustomobject]@{ Name = 'CPACR FPEN=0b11 (EL0-FP frei)';         Pattern = 'CPACR_EL1\.FPEN=0b11' }
    [pscustomobject]@{ Name = 'FP-Kontext-Guardian (2x FPTEST/Kern1)'; Pattern = '\[vk\] T3\.1 FP-Kontext-Guardian' }
    [pscustomobject]@{ Name = 'r3d-Rasterizer-Selbsttest ok';          Pattern = '\[vktest\] r3d: flat=ok' }
    [pscustomobject]@{ Name = 'Vulkan-Kern fertig';                    Pattern = '\[vktest\] vk-kern fertig' }
    [pscustomobject]@{ Name = 'SPIR-V-Interpreter fertig';             Pattern = '\[vktest\] spirv fertig' }
    [pscustomobject]@{ Name = 'Draw-Pfad fertig (inkl. V1.1-1.10)';    Pattern = '\[vktest\] vkdraw fertig' }
    [pscustomobject]@{ Name = 'VKCUBE 12 Frames praesentiert';         Pattern = '\[vkcube\] probe nach 12 frames' }
    [pscustomobject]@{ Name = 'VKTEST stufe1 fertig (exit 0)';         Pattern = '\[vktest\] stufe1 fertig' }
    [pscustomobject]@{ Name = 'Scheduler gestartet (alle Kerne)';      Pattern = '\[8\] Scheduler starten' }
)
# Fail-fast NUR auf echte Halts/Panics ODER einen Vk-EIGENEN Fehlermarker -- NICHT auf harmloses
# "fehlgeschlagen" im Standard-Scaffolding (RTOS_SELFTEST-Boot).
$VkHaltPattern = 'KERNEL-HALT|KERNEL PANIC|\[vk\] FEHLER|\[vktest\].*FEHLER|\[vkcube\].*FEHLER'
# -Vk baut MIT RTOS_SELFTEST (Scaffolding erwartet) -> kein grep-clean-Check.
$VkForbidden = @()
