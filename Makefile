# rpi_rtos -- Makefile
#
# Voraussetzungen: AArch64-Bare-Metal-Toolchain (Standard: aarch64-none-elf-)
# und qemu-system-aarch64. Anderer Prefix z.B.:  make CROSS=aarch64-linux-gnu-

CROSS   ?= aarch64-none-elf-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
QEMU    ?= qemu-system-aarch64

CFLAGS  := -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only \
           -mcpu=cortex-a72 -O2 -Wall -Wextra \
           -fno-pie -fno-pic -fno-stack-protector \
           -MMD -MP \
           -Iinclude -Idrivers/uart
LDFLAGS := -no-pie
LDSCRIPT := arch/aarch64/linker.ld

# Portabler Netz-Stack (von raspi4b- und virt-Build gemeinsam genutzt).
NET_OBJS := net/net.o net/arp.o net/ip.o net/icmp.o net/udp.o \
            net/tcp.o net/dhcp.o net/dns.o net/http.o net/httpd.o lib/kmem.o

OBJS := arch/aarch64/start.o \
        arch/aarch64/vectors.o \
        arch/aarch64/switch.o \
        arch/aarch64/mmu.o \
        arch/aarch64/exceptions.o \
        drivers/uart/uart.o \
        drivers/gic/gic.o \
        drivers/timer/timer.o \
        drivers/sd/sd.o \
        drivers/net/genet.o \
        drivers/gpio/gpio.o \
        drivers/spi/spi.o \
        drivers/i2c/i2c.o \
        drivers/pwm/pwm.o \
        drivers/mailbox/mailbox.o \
        drivers/video/fb.o \
        drivers/video/fbcon.o \
        drivers/video/gui_fb.o \
        drivers/usb/dwc2.o \
        drivers/usb/usbkbd.o \
        drivers/usb/usbmsc.o \
        fs/fat32.o \
        fs/vfs.o \
        net/httpd_fs.o \
        lib/sha256.o \
        kernel/user.o \
        kernel/sched.o \
        kernel/ipc.o \
        kernel/elf.o \
        kernel/syscall.o \
        kernel/smp.o \
        kernel/proc.o \
        kernel/kmain.o \
        $(NET_OBJS)

# virt-Netz-Harness (Verifikation des Stacks ueber virtio-net auf QEMU `virt`).
VIRT_OBJS := arch/aarch64/start.o \
             boards/virt/trap_virt.o \
             boards/virt/uart_virt.o \
             boards/virt/mmu_virt.o \
             boards/virt/main_virt.o \
             drivers/net/virtio_net.o \
             boards/virt/smp_stub.o \
             $(NET_OBJS)
# Build-Ausgaben gebuendelt unter _build/ (ELFs/Images/Tools/fwtmp an einem Ort).
OUTDIR   := _build
VIRT_ELF      := $(OUTDIR)/net_virt.elf
VIRT_LDSCRIPT := boards/virt/linker_virt.ld

IMG      := $(OUTDIR)/kernel8.img
ELF      := $(OUTDIR)/kernel8.elf
USER_ELF := $(OUTDIR)/hello.elf
SDIMG    := $(OUTDIR)/sd.img

.PHONY: all run clean sdimg virt virt-run

all: $(IMG) $(USER_ELF)

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(ELF): $(OBJS) $(LDSCRIPT) | $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS)

$(IMG): $(ELF)
	$(OBJCOPY) -O binary $< $@
	@echo "Build OK -> $(IMG)"

# User-Applikation (EL0, ELF64) auf USER_BASE gelinkt.
$(USER_ELF): user/hello.c user/user.ld | $(OUTDIR)
	$(CC) $(CFLAGS) -c user/hello.c -o user/hello.o
	$(LD) -no-pie -z max-page-size=4096 -T user/user.ld -o $(USER_ELF) user/hello.o

# FAT32-SD-Image mit eingebetteter User-App (INIT.ELF) erzeugen.
sdimg: $(USER_ELF)
	python _build/tools/gen_sdimg.py $(SDIMG) $(USER_ELF)

# In QEMU starten (benoetigt QEMU >= 9.0 fuer die Maschine raspi4b).
# PL011 ist in QEMU fest serial0, daher trifft '-serial stdio' die UART korrekt.
# Fuer zusaetzliches Monitor-Multiplex (Strg-A, dann C):  make run QEMUSERIAL='-serial mon:stdio'
QEMUSERIAL ?= -serial stdio
run: $(IMG) sdimg
	$(QEMU) -M raspi4b -kernel $(IMG) -drive file=$(SDIMG),if=sd,format=raw $(QEMUSERIAL) -display none

# --- virt-Netz-Harness ---
# QEMU `virt` laedt das ELF gemaess Programm-Header (Eintritt = _start @0x40100000).
virt: $(VIRT_ELF)

$(VIRT_ELF): $(VIRT_OBJS) $(VIRT_LDSCRIPT) | $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -T $(VIRT_LDSCRIPT) -o $@ $(VIRT_OBJS)
	@echo "Build OK -> $(VIRT_ELF)"

# Interaktiv starten; SLIRP-User-Net mit UDP-Echo-Portweiterleitung.
virt-run: $(VIRT_ELF)
	$(QEMU) -M virt -cpu cortex-a72 -m 256M -kernel $(VIRT_ELF) \
	  -global virtio-mmio.force-legacy=false \
	  -netdev user,id=n0,hostfwd=udp::5555-:5555 \
	  -device virtio-net-device,netdev=n0 -serial stdio -display none

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(VIRT_OBJS) $(VIRT_OBJS:.o=.d) \
	      $(ELF) $(IMG) $(VIRT_ELF) user/hello.o $(USER_ELF) $(SDIMG)

# Automatisch erzeugte Header-Abhaengigkeiten (-MMD -MP) einbinden.
-include $(OBJS:.o=.d) $(VIRT_OBJS:.o=.d)
