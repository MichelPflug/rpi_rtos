/*
 * include/abi.h  --  User<->Kernel-ABI: Syscall-Nummern (von Kernel UND User-App genutzt)
 *
 * Aufrufkonvention (AArch64): Syscall-Nummer in x8, Argumente in x0..x5,
 * Rueckgabewert in x0, Ausloesung per `svc #0`.
 */
#ifndef RPI_RTOS_ABI_H
#define RPI_RTOS_ABI_H

#define SYS_EXIT      0   /* (code)            -> kehrt nie zurueck            */
#define SYS_WRITE     1   /* (fd, buf, len)    -> Anzahl geschriebener Bytes  */
#define SYS_YIELD     2   /* ()                -> CPU abgeben                  */
#define SYS_SLEEP_MS  3   /* (ms)              -> schlafen                     */
#define SYS_GETPID    4   /* ()                -> monotone PID des Aufrufers (nie recycled, != tasks[]-Slot) */
#define SYS_READ_FILE 5   /* (path, buf, max)  -> gelesene Bytes / -1          */
#define SYS_WRITE_FILE 6  /* (path, buf, len)  -> geschriebene Bytes / -1      */
#define SYS_WHOAMI    7   /* ()                -> uid des aufrufenden Prozesses */
#define SYS_USERADD   8   /* (name, password)  -> 0 ok / -1 (braucht Admin-Cap) */
#define SYS_READ      9   /* (fd, buf, max)    -> Zeile von Konsole lesen, Laenge */
#define SYS_SPAWN    10   /* (path, core)      -> Programm starten (erbt cred); core=Kern-
                           *                      Affinitaet (0..3), tid/-1 */
#define SYS_LISTDIR  11   /* (mount, buf, max) -> Verzeichnis als Text, Laenge / -1   */
#define SYS_DELETE   12   /* (path)            -> Datei loeschen, 0 / -1 (nur hdd1/2)  */
#define SYS_PASSWD   13   /* (newpw)           -> eigenes Passwort aendern, 0 / -1     */
#define SYS_MKDIR    14   /* (path)            -> Verzeichnis anlegen, 0 / -1 (hdd1/2)  */
#define SYS_RMDIR    15   /* (path)            -> leeres Verzeichnis entfernen, 0 / -1  */
#define SYS_GETCPU   16   /* ()                -> Kern-ID (MPIDR.Aff0), auf dem der Task laeuft */
#define SYS_READCHAR 17   /* ()                -> EIN rohes Konsolen-Byte (blockierend, kein Echo,
                           *                      keine Zeilenpufferung) fuer den Shell-Editor   */
#define SYS_WAIT     18   /* (pid)             -> blockiert bis das Kind 'pid' endet; Exit-Code / -1
                           *                      (nur eigene Kinder; 137 = gekillt, 139 = Fault)  */
#define SYS_KILL     19   /* (pid)             -> Prozess 'pid' beenden, 0 / -1. Erlaubt fuer eigene
                           *                      Kinder oder mit Admin-Cap; Wirkung am Safe-Point  */
#define SYS_GETPPID  20   /* ()                -> PID des Elternprozesses (0 = kernel-gestartet)    */
#define SYS_GUI_INFO 21   /* (info*)           -> Framebuffer-Geometrie + EL0-Backbuffer-VA in *info,
                           *                      0 / -1 (T2.1: Grafik-Bruecke fuer GUI-Apps)        */
#define SYS_GUI_FLUSH 22  /* (y, nrows)        -> Backbuffer-Zeilen [y,y+n) -> echter FB + Cache-
                           *                      Flush fuer die GPU, 0 / -1                          */
#define SYS_POLL_EVENT 23 /* (event*)          -> naechstes GUI-Eingabe-Event non-blocking: 1 = Event
                           *                      in *event, 0 = keins, -1 (braucht USER_CAP_GUI)     */
#define SYS_WAIT_EVENT 24 /* (event*)          -> blockiert bis ein GUI-Event vorliegt: 1, -1        */
/* VISION (docs/architecture/19, A1.5): Kernel-Parallel-For fuer die KI-Inferenz. Nur im
 * -Vision-Build wirksam (Kernel-Dispatch + EL0-Wrapper je #ifdef VISION); die Konstanten selbst
 * erzeugen keinen Code -> Kernel ohne das Flag byte-identisch. */
#define SYS_VI_PARSPAWN    25 /* (fn, arg, n)      -> Worker fuer wid=1..n-1 starten; Anzahl        */
#define SYS_VI_PARJOIN     26 /* ()                -> auf alle Worker warten (Barrier)              */
#define SYS_VI_WORKER_DONE 27 /* ()                -> Worker: Fertigmeldung + Task-Ende (kein Ret)  */
#define SYS_VI_CAM_GRAB    28 /* (buf, max)        -> ein Kamera-Frame greifen; Bytes / -1 (kein Geraet).
                               *                      QEMU: kein UVC-Geraet -> -1; echter UVC-Treiber = Pi4 */
#define SYS_VI_TICKS       29 /* ()                -> monotoner Zaehler CNTPCT_EL0 (fuer A5.2-FPS-Messung;
                               *                      EL0 liest die Frequenz aus CNTFRQ_EL0)              */

#endif /* RPI_RTOS_ABI_H */
