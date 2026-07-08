/*
 * drivers/uart/uart.c  --  PL011 (UART0) Treiber fuer BCM2711
 *
 * Basis 0xFE201000 (Low-Peripheral-Modus). Auf echter Hardware muss PL011 per
 * config.txt (dtoverlay=disable-bt) auf GPIO14/15 gelegt werden; in QEMU ist
 * UART0 bereits mit der seriellen Konsole verbunden.
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "spinlock.h"
#include "uart.h"

#ifdef DEV_REMOTE
void dev_console_tee(char c);   /* net/dev_agent.c: Konsolen-Byte -> Dev-Remote-OUTPUT-Ring */
#endif
#ifdef DIAG_LOG
void diag_log_putc(char c);     /* kernel/diag_log.c: Konsolen-Byte -> Boot-Log-RAM-Puffer (-> SD) */
#endif

/* Konsolen-Ausgabe-Lock: serialisiert ganze Strings/Puffer ueber ALLE Kerne (sonst
 * verschraenken sich Zeichen verschiedener Kerne -- sichtbar, sobald EL0-Prozesse auf
 * Sekundaerkernen zur Laufzeit drucken). REENTRANT auf demselben Kern: uart_begin/_end
 * klammern eine mehrteilige Zeile (z.B. uart_puts + uart_putdec + ...) zu EINER atomaren
 * Ausgabe; die put*-Funktionen selbst klammern intern ebenfalls -> kein Self-Deadlock.
 * Beim Halten sind IRQs maskiert (Owner = dieser Kern), daher ist die Reentranz-Pruefung
 * (s_uart_owner == cpu_id()) rennfrei: nur der Owner schreibt seine eigene ID. */
static spinlock_t      s_uartlock = SPINLOCK_INIT;
static volatile int    s_uart_owner = -1;   /* haltender Kern (-1 = frei) */
static int             s_uart_depth;        /* Reentranz-Tiefe (nur vom Owner angefasst) */
static uint64_t        s_uart_flags;        /* DAIF des aeussersten Halters */

/* Der Spinlock nutzt LDAXR/STXR (Load/Store-Exclusive). Die funktionieren auf echter HW (Cortex-A72)
 * NUR mit eingeschalteter MMU + Caches (Normal-cacheable Speicher); mit MMU AUS schlaegt STXR immer
 * fehl -> der Lock dreht endlos. Der fruehe Boot (Banner + [1]/[2]-Ausgaben) laeuft VOR mmu_init und
 * ist dort ohnehin single-core -> KEIN Lock. Erst nach mmu_init (uart_lock_online) wird er scharf. */
static volatile int    s_uart_lock_on;      /* 0 = vor mmu_init, Spinlock aus (kein LDAXR/STXR) */

void uart_lock_online(void) { s_uart_lock_on = 1; }

void uart_begin(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    int me = (int)cpu_id();
    if (s_uart_owner == me) {        /* schon von diesem Kern gehalten -> nur vertiefen */
        s_uart_depth++;
        return;
    }
    if (s_uart_lock_on) {            /* erst nach mmu_init: MMU an -> LDAXR/STXR funktioniert */
        spin_lock(&s_uartlock);
    }
    s_uart_owner = me;
    s_uart_depth = 1;
    s_uart_flags = f;                /* DAIF des aeussersten begin merken */
}

void uart_end(void)
{
    if (--s_uart_depth == 0) {
        uint64_t f = s_uart_flags;
        s_uart_owner = -1;
        if (s_uart_lock_on) {
            spin_unlock(&s_uartlock);
        }
        WRITE_SYSREG(daif, f);
    }
}

/* --- BCM2711 Basisadressen (Low-Peripheral-Modus) --- */
#define PERIPHERAL_BASE   0xFE000000UL
#define GPIO_BASE         (PERIPHERAL_BASE + 0x200000UL)
#define UART0_BASE        (PERIPHERAL_BASE + 0x201000UL)

/* --- GPIO --- */
#define GPFSEL1                 (GPIO_BASE + 0x04UL)   /* GPIO 10..19 */
#define GPIO_PUP_PDN_CNTRL_REG0 (GPIO_BASE + 0xE4UL)   /* BCM2711-Pull-Schema */

/* --- PL011 Register (Offsets relativ zu UART0_BASE) --- */
#define UART0_DR    (UART0_BASE + 0x00UL)
#define UART0_FR    (UART0_BASE + 0x18UL)
#define UART0_IBRD  (UART0_BASE + 0x24UL)
#define UART0_FBRD  (UART0_BASE + 0x28UL)
#define UART0_LCRH  (UART0_BASE + 0x2CUL)
#define UART0_CR    (UART0_BASE + 0x30UL)
#define UART0_IMSC  (UART0_BASE + 0x38UL)
#define UART0_ICR   (UART0_BASE + 0x44UL)

#define FR_TXFF     (1u << 5)   /* TX-FIFO voll */
#define FR_RXFE     (1u << 4)   /* RX-FIFO leer */
#define FR_BUSY     (1u << 3)   /* UART sendet gerade */

void uart_init(void)
{
    /* 1) UART deaktivieren, bevor wir konfigurieren. */
    mmio_write32(UART0_CR, 0);

    /* Laufende Uebertragung abwarten (nur bei Re-Init zur Laufzeit relevant) -- BESCHRAENKT: bleibt
     * BUSY auf echter HW haengen (UART nicht sauber getaktet), darf das uart_init nicht aufhaengen. */
    { uint32_t to = 0x40000u; while ((mmio_read32(UART0_FR) & FR_BUSY) && --to) { } }

    /* 2) GPIO14/15 auf ALT0 (PL011 TXD0/RXD0) legen. */
    uint32_t sel = mmio_read32(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));   /* FSEL14 / FSEL15 loeschen */
    sel |=  ((4u << 12) | (4u << 15));   /* ALT0 = 0b100 */
    mmio_write32(GPFSEL1, sel);

    /* Pull-Widerstaende fuer GPIO14/15 deaktivieren (00 = kein Pull). */
    uint32_t pud = mmio_read32(GPIO_PUP_PDN_CNTRL_REG0);
    pud &= ~((3u << 28) | (3u << 30));
    mmio_write32(GPIO_PUP_PDN_CNTRL_REG0, pud);

    /* 3) Anstehende Interrupts loeschen. */
    mmio_write32(UART0_ICR, 0x7FF);

    /* 4) Baudrate 115200 bei 48 MHz Referenztakt:
     *    DIV = 48e6 / (16 * 115200) = 26.0416..
     *    -> IBRD = 26, FBRD = round(0.0416 * 64) = 3
     *    (In QEMU ohne Belang; auf HW ggf. UART-Clock per Mailbox pinnen.)
     */
    mmio_write32(UART0_IBRD, 26);
    mmio_write32(UART0_FBRD, 3);

    /* 5) Leitungsformat: 8N1, FIFOs aktiv (WLEN=0b11<<5 | FEN<<4). */
    mmio_write32(UART0_LCRH, (3u << 5) | (1u << 4));

    /* 6) Alle Interrupts maskieren (Polling-Betrieb in M0). */
    mmio_write32(UART0_IMSC, 0);

    /* 7) UART + TX + RX aktivieren (UARTEN | TXE | RXE). */
    mmio_write32(UART0_CR, (1u << 0) | (1u << 8) | (1u << 9));
}

/* Optionaler Ausgabe-Spiegel (z.B. die HDMI-Textkonsole): jedes ausgegebene
 * Zeichen wird zusaetzlich hierhin geleitet. NULL = nur serielle Ausgabe. */
static void (*s_mirror)(char c);

/* 1 = TX-FIFO leert nie (UART auf echter HW nicht funktionsfaehig: nicht getaktet/geroutet). Wird
 * EINMAL beim ersten Timeout gesetzt -> danach kein Warten/Senden mehr, damit eine tote UART nicht
 * jedes einzelne Zeichen (und damit den ganzen Boot) ausbremst. In QEMU leert die FIFO -> bleibt 0. */
static int s_uart_dead;

void uart_set_mirror(void (*fn)(char c))
{
    s_mirror = fn;
}

/* Ein Zeichen rohe ausgeben (OHNE Lock). Nur aus gelockten Wrappern aufrufen. */
static void putc_raw(char c)
{
    /* Auf Platz in der TX-FIFO warten -- beschraenkt. Leert sie sich nie (tote UART), wird das EINMAL
     * erkannt (s_uart_dead) und ab dann uebersprungen -> der Boot laeuft in voller Geschwindigkeit
     * weiter, Ausgabe geht nur noch auf den HDMI-Spiegel. In QEMU leert die FIFO -> nie tot. */
    if (!s_uart_dead) {
        uint32_t to = 0x40000u;
        while ((mmio_read32(UART0_FR) & FR_TXFF) && --to) {
            /* warten, bis Platz im TX-FIFO ist (bounded) */
        }
        if (to == 0u) {
            s_uart_dead = 1;      /* FIFO leert nie -> UART tot, ab jetzt nicht mehr warten/senden */
        } else {
            mmio_write32(UART0_DR, (uint32_t)(unsigned char)c);
        }
    }

    /* HDMI-Spiegel (fbcon) NUR von Kern 0: der fbcon-Zustand (Cursor/Scroll/ANSI) ist nicht
     * reentrant -> Ausgabe von einem Sekundaerkern wuerde ihn korrumpieren. Sekundaerkern-
     * Ausgabe geht auf die serielle UART (jetzt per s_uartlock string-atomar serialisiert). */
    if (s_mirror && cpu_id() == 0) {
        s_mirror(c);
    }
#ifdef DEV_REMOTE
    /* Dev-Remote (docs/architecture/20): jedes Konsolen-Byte zusaetzlich in den OUTPUT-Ring tee'en
     * (best-effort, non-blocking); der Netz-Task streamt es an den Controller. Nur Kern 0 (wie der
     * Mirror). Ganz #ifdef DEV_REMOTE -> ohne das Flag kein Byte. */
    if (cpu_id() == 0) {
        dev_console_tee(c);
    }
#endif
#ifdef DIAG_LOG
    /* Boot-Log-Mitschnitt in RAM (nur Kern 0) -> spaeter nach hdd1:BOOTLOG.TXT. Ganz #ifdef DIAG_LOG. */
    if (cpu_id() == 0) {
        diag_log_putc(c);
    }
#endif
}

static void puts_raw(const char *s)
{
    for (; *s != '\0'; ++s) {
        if (*s == '\n') {
            putc_raw('\r');   /* CR vor LF fuer Terminals */
        }
        putc_raw(*s);
    }
}

/* Gesetzt, sobald ein Kern in panic() ist (Definition in exceptions.c). Waehrend einer Panik
 * unterdruecken die gelockten Normalausgaben ihre Ausgabe, damit der (lock-freie) Panik-Dump
 * eines noch nicht gestoppten Kerns nicht zeichenweise zerstueckelt wird. */
extern volatile int g_panicking;

void uart_putc(char c)
{
    if (g_panicking) { return; }
    uart_begin();
    putc_raw(c);
    uart_end();
}

/* --- Lock-freie Notausgabe fuer panic()/Fault-Halt ---
 * Der UART-Lock koennte gerade vom fehlerhaften Kern gehalten werden -> im Panik-Pfad NICHT
 * nehmen (sonst Self-Deadlock). Ausgabe geht direkt ueber putc_raw/puts_raw (ungelockt). */
void uart_panic_puts(const char *s)
{
    puts_raw(s);
}

void uart_panic_hex(unsigned long long v)
{
    puts_raw("0x");
    for (int i = 60; i >= 0; i -= 4) {
        putc_raw("0123456789ABCDEF"[(v >> i) & 0xF]);
    }
}

/* Rohpuffer (len Bytes, KEINE \n-Uebersetzung) string-atomar ausgeben -- fuer EL0
 * sys_write, damit die Ausgabe eines Prozesses nicht mit der eines anderen Kerns
 * verschraenkt. */
void uart_write(const char *buf, uint32_t len)
{
    if (g_panicking) { return; }
    uart_begin();
    for (uint32_t i = 0; i < len; i++) {
        putc_raw(buf[i]);
    }
    uart_end();
}

char uart_getc(void)
{
    while (mmio_read32(UART0_FR) & FR_RXFE) {
        /* warten, bis ein Zeichen im RX-FIFO liegt */
    }
    return (char)(mmio_read32(UART0_DR) & 0xFF);
}

int uart_getc_nb(void)
{
    if (mmio_read32(UART0_FR) & FR_RXFE) {
        return -1;                    /* RX-FIFO leer */
    }
    return (int)(mmio_read32(UART0_DR) & 0xFF);
}

void uart_puts(const char *s)
{
    if (g_panicking) { return; }
    uart_begin();
    puts_raw(s);
    uart_end();
}

void uart_puthex(unsigned long long value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (g_panicking) { return; }
    uart_begin();
    puts_raw("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        putc_raw(hex[(value >> shift) & 0xF]);
    }
    uart_end();
}

void uart_putdec(unsigned long long value)
{
    char buf[20];
    int i = 0;
    if (g_panicking) { return; }
    uart_begin();
    if (value == 0) {
        putc_raw('0');
        uart_end();
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i-- > 0) {
        putc_raw(buf[i]);
    }
    uart_end();
}
