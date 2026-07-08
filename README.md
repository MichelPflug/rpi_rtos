# rpi_rtos

**rpi_rtos** ist ein experimentelles 64-Bit-Bare-Metal-RTOS für den Raspberry Pi 4 und QEMU `raspi4b`.  
Das Projekt kombiniert einen eigenen AArch64-Kernel, ein kleines Userland, eine einfache GUI-Schicht und spätere Forschungszweige für KI-Bildauswertung sowie Vulkan-/3D-Rendering.

> Status: Forschungs- und Lernprojekt. Nicht als produktionsreifes Betriebssystem oder sicherheitskritische Plattform gedacht.

---

## Highlights

- **AArch64 Bare-Metal-Kernel** für Raspberry Pi 4 / Cortex-A72
- **MMU, Caches, Exception-Vektoren und EL0-Userland**
- **Präemptiver SMP-Scheduler** mit festen Core-Affinitäten
- **EL0-Prozesse** mit eigenem Adressraum, Syscalls, PIDs, `spawn`, `wait`, `kill` und Prozessrechten
- **Framebuffer-Konsole und GUI-Bridge** für Userland-Anwendungen
- **WinForms-ähnliche GUI-Bibliothek** mit Controls, Fokus, Tastatur- und Maus-Events
- **FAT32-/VFS-Schicht** für System- und User-Partitionen
- **Treiber-Bausteine** für UART, Timer, GIC, GPIO, SPI, I2C, PWM, Mailbox, SD, USB und Netzwerk
- **Minimaler Netzwerk-Stack** mit ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP und HTTP-Komponenten
- **QEMU-Harnesses** für Raspberry-Pi- und `virt`-Builds
- **AI-Vision-Modul** als gekapselte EL0-Selbsttest-App mit einfacher Inferenz-/Bildverarbeitungs-Pipeline
- **Vulkan-/3D-Track** mit Software-Rasterizer, Vulkan-ähnlicher API-Schicht, SPIR-V-Verarbeitung und V3D-Bring-up-Pfad

---

## Repository-Struktur

```text
arch/aarch64/        AArch64-Startcode, Exception-Vektoren, MMU, Kontextwechsel
boards/virt/         QEMU-virt-Harness für Netzwerk-/Interop-Tests
boot/                Raspberry-Pi-Boot-Konfigurationen
kernel/              Kernel-Einstieg, Scheduler, Syscalls, Prozesse, IPC, SMP, GUI-Input
drivers/             UART, GIC, Timer, SD, USB, Video, Netzwerk, GPIO/SPI/I2C/PWM, PCIe, V3D
fs/                  FAT32 und VFS
net/                 ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, HTTP
include/             Öffentliche Kernel-, Treiber- und Userland-Header
lib/                 Kernel-nahe Hilfsbibliotheken
user/                EL0-Programme, Shell, GUI-Demos, AI-Vision, Vulkan-/3D-Demos
user/lib/            Userland-Bibliotheken für GUI, WinForms, Rasterizer, Vision und Vulkan
build.ps1            Windows-Buildscript mit vielen Build-Flavors und Verify-Modi
Makefile             Minimaler GNU-Make-Buildpfad
run_*.ps1            Komfortstarter für Shell, GUI und Vulkan-Demos
```

---

## Voraussetzungen

### Windows

- PowerShell 5+ oder PowerShell 7+
- QEMU mit `qemu-system-aarch64`
- Eine AArch64-Toolchain, zum Beispiel:
  - `aarch64-none-elf-gcc`, oder
  - `aarch64-linux-gnu-gcc`, oder
  - LLVM/Clang mit `ld.lld` und `llvm-objcopy`

Die Skripte versuchen typische Installationspfade wie `C:\Program Files\LLVM\bin` und `C:\Program Files\qemu` automatisch in den `PATH` aufzunehmen.

### Linux / WSL

- `make`
- `qemu-system-aarch64`
- AArch64-GNU-Toolchain, standardmäßig `aarch64-none-elf-`

---

## Build & Run

### Windows: Shell starten

```powershell
.\run_shell.ps1
```

Startet ein loginfähiges Image in QEMU über die serielle Konsole.

Standard-Login im Demo-/Test-Image:

```text
login:    admin
password: admin
```

Beim ersten Login wird ein neues Passwort verlangt und in der SD-Image-Datei gespeichert.

### Windows: GUI starten

```powershell
.\run_gui.ps1
```

Baut ein GUI-Image und startet QEMU mit HDMI-Fenster. Die Tastatursteuerung läuft zuverlässig über das Terminal; Maus-Events werden in QEMU emuliert.

### Windows: Vulkan-/3D-Demo starten

```powershell
.\run_vk.ps1
```

Startet den Vulkan-/3D-Selbsttest und anschließend eine rotierende Cube-Demo.

### Windows: Vulkan in GUI starten

```powershell
.\run_vkgui.ps1
```

Startet eine WinForms-ähnliche GUI mit eingebettetem, live gerendertem Vulkan-/3D-Viewport.

### Direkter Build über PowerShell

```powershell
.\build.ps1                 # Standard-Build
.\build.ps1 -Run            # Build + QEMU raspi4b
.\build.ps1 -Login          # Interaktiver Login
.\build.ps1 -GuiLogin       # Login -> GUI-Sitzung
.\build.ps1 -Vk             # Vulkan-/3D-Selbsttest
.\build.ps1 -Vision         # AI-Vision-Selbsttest
.\build.ps1 -Virt -Verify   # QEMU-virt-Netzwerk-Harness mit Verifikation
.\build.ps1 -Release        # Sauberes Release-Image ohne Selbsttest-Scaffolding
```

### GNU Make

```bash
make
make run
make virt
make virt-run
make clean
```

Der Makefile-Pfad ist bewusst kleiner gehalten als `build.ps1` und deckt vor allem den klassischen Kernel-/QEMU-Pfad ab.

---

## Build-Flavors

Einige wichtige `build.ps1`-Flags:

| Flag | Zweck |
| --- | --- |
| `-Login` | Interaktiver Login über serielle Konsole |
| `-Release` | Produktionsnahes Image ohne Selbsttest-Scaffolding |
| `-GuiApp` | GUI-App direkt im Selbsttest-Boot starten |
| `-GuiLogin` | Login und danach GUI-Sitzung starten |
| `-Vk` | Vulkan-/3D-Selbsttests und Demo aktivieren |
| `-VkGuiApp` | Vulkan-/3D-Demo in der GUI starten |
| `-Vision` | AI-Vision-Modul und Vision-Selbsttest aktivieren |
| `-V3d` | V3D-Hardware-Probe für echten Raspberry Pi 4 aktivieren |
| `-Virt -Verify` | Netzwerkstack in QEMU `virt` automatisiert prüfen |
| `-DevImage` | Entwicklerimage mit Auto-Login, Vision, Vulkan und Dev-Remote |

---

## Architekturüberblick

### Kernel

Der Kernel startet auf AArch64, initialisiert Exception-Vektoren, MMU, Caches, Timer, GIC, Framebuffer, Storage und optionale Diagnosepfade. Danach werden Scheduler, IPC, Userland und je nach Build-Flavor Shell, GUI oder Testprozesse gestartet.

### Scheduler und Prozesse

Der Scheduler arbeitet präemptiv und partitioniert pro Core. Prozesse laufen in EL0, erhalten eigene Adressräume und werden über Syscalls kontrolliert. Prozessrechte steuern unter anderem den Zugriff auf GUI-Funktionen.

### Dateisystem und Userland

Das System nutzt eine kleine VFS-Schicht mit FAT32-Backend. Userprogramme werden als ELF-Dateien von der Systempartition geladen. Die Shell bietet einfache Kommandos und kann weitere Programme starten.

### GUI

Die GUI-Schicht verbindet einen Kernel-Framebuffer-Backbuffer mit EL0-Anwendungen. Darauf baut eine einfache WinForms-ähnliche Userland-Bibliothek mit Controls, Fokus-Handling, Tastatur- und Maus-Events auf.

### AI Vision

Der Vision-Track ist per Build-Flag gekapselt. Ohne `-Vision` wird der Vision-Code nicht in das Image eingebunden. Mit aktiviertem Vision-Build startet eine EL0-Testanwendung für Bildauswertung, einfache Inferenz und parallele Worker-Pfade.

### Vulkan / 3D

Der Vulkan-/3D-Track enthält eine Vulkan-ähnliche RTOS-API-Schicht, SPIR-V-Verarbeitung, einen Software-Rasterizer und Demo-Anwendungen. Der V3D-Pfad dient als Bring-up- und Hardware-Erkennungsbasis für den Raspberry Pi 4.

---

## Sicherheitshinweise

- Demo-/Selbsttest-Images können Testkonten, Auto-Login oder Dev-Remote-Funktionen enthalten.
- `-DevImage` ist ausdrücklich nur für Entwicklung und Diagnose gedacht.
- Für saubere Builds ohne Selbsttest-/Demo-Scaffolding sollte `-Release` verwendet werden.
- Das Projekt ist experimentell und nicht für produktive oder sicherheitskritische Einsätze vorgesehen.

---
