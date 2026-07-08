/*
 * include/sched.h  --  Preemptiver Festprioritaeten-Scheduler (Round-Robin pro Stufe)
 */
#ifndef RPI_RTOS_SCHED_H
#define RPI_RTOS_SCHED_H

#include <stdint.h>

typedef void (*task_entry_t)(void *arg);

/* Niedrigere Zahl = hoehere Prioritaet (0 = hoechste). */

void sched_init(void);

#if defined(RTOS_SELFTEST) && defined(GUI_FP)
/* Guardian (white-box): beweist die fpctx-Zero-Init bei Slot-Vergabe deterministisch
 * (first-fit-Slot vergiften -> Task anlegen -> fpctx muss genullt sein -> Slot freigeben).
 * VOR sched_start() und ohne parallele task_creates aufrufen. 1 = ok. */
int sched_fpctx_zeroinit_selftest(void);
#endif

/* Erzeugt einen Kernel-Thread (Affinitaet = Kern 0). Task-ID (>=0) oder -1. */
int  task_create(task_entry_t entry, void *arg, uint8_t priority, const char *name);

/* Wie task_create, aber bindet den Task an einen bestimmten Kern (SMP-Affinitaet).
 * Jeder Kern schedulet ausschliesslich seine eigenen Tasks (partitioniert). EL0-/
 * User-Prozesse muessen auf Kern 0 bleiben (gemeinsame User-VA-Kachel). */
int  task_create_on(uint32_t core, task_entry_t entry, void *arg,
                    uint8_t priority, const char *name);

/* Wie task_create_on, aber NICHT-schedulebar (TASK_SETUP). Der Aufrufer setzt ttbr/cred
 * und ruft dann task_admit(tid). Schliesst das Laufzeit-Spawn-Fenster, in dem ein
 * Sekundaerkern einen User-Task mit noch nicht gesetztem TTBR0 einplanen koennte. */
int  task_create_suspended_on(uint32_t core, task_entry_t entry, void *arg,
                              uint8_t priority, const char *name);

/* Gibt einen mit task_create_suspended_on angelegten Task frei (mit maskierten IRQs
 * aufrufen). Publiziert ttbr/cred (Release) und weckt bei fremder Affinitaet den
 * Eigentuemer-Kern per Reschedule-IPI. */
void task_admit(int tid);

/* Startet den Scheduler auf Kern 0 und springt in den ersten Task. Kehrt nie zurueck. */
void sched_start(void);

/* Startet den Scheduler auf einem Sekundaerkern (laeuft dessen eigene Tasks/Idle).
 * Kehrt nie zurueck. Von secondary_main aufgerufen, nachdem Core 0 die Tasks angelegt
 * und die Freigabe signalisiert hat. */
void sched_start_secondary(void);

/* Freiwillige Abgabe der CPU. */
void sched_yield(void);

/* Aktuellen Task fuer 'ticks' Timer-Ticks schlafen legen. */
void task_sleep_ticks(uint64_t ticks);

/* Beendet den aktuellen Task mit Exit-Code (kehrt nie zurueck). Fuer einen EL0-Prozess mit
 * Elternprozess (ppid != 0) wird der Code in einem Exit-Record hinterlegt und ein in
 * sched_wait_pid() wartender Elternprozess geweckt. */
void task_exit(int code);

/* Exit-Codes fuer nicht-normale Beendigung (Konvention 128 + Signalnummer). */
#define TASK_EXIT_KILLED 137          /* per SYS_KILL beendet (128 + 9)  */
#define TASK_EXIT_FAULT  139          /* durch EL0-Fault beendet (128 + 11) */

/* --- Prozess-Handles (wait/kill), fuer die EL0-Syscalls SYS_WAIT/SYS_KILL/SYS_GETPPID --- */
/* Eltern-PID eines Tasks setzen (beim Spawn, waehrend TASK_SETUP). 0 = kein Elternprozess. */
void     sched_set_ppid(int tid, uint64_t ppid);
/* Eltern-PID des laufenden Tasks (SYS_GETPID-Pendant). 0 = kernel-gestartet/keiner. */
uint64_t sched_current_ppid(void);
/* Blockiert den aufrufenden Task, bis sein Kind 'pid' endet; liefert dessen Exit-Code (>=0)
 * bzw. -1 (kein eigenes, lebendes/beendetes Kind mit dieser PID). Nimmt intern s_ipclock. */
long     sched_wait_pid(uint64_t pid);
/* Markiert den Prozess 'pid' zur Beendigung (kill_pending) -> er endet am naechsten Safe-Point
 * (Syscall-Eintritt bzw. IRQ-Rueckkehr nach EL0). Erlaubt, wenn der Aufrufer der Elternprozess
 * ist ODER Admin. 0 / -1. */
int      sched_kill_pid(uint64_t pid, uint64_t caller_pid, int caller_is_admin);
/* An einem Safe-Point (EL0-Grenze) aufgerufen: wenn der laufende Task kill_pending traegt,
 * beendet er sich (task_exit, kehrt nie zurueck). */
void     sched_exit_if_killed(void);

/* Markiert Task 'tid' als User-Prozess: physischer Bereich 'user_phys' (fuer den
 * Exit-Hook) + 'ttbr' (TTBR0 des Prozess-Adressraums, vom Scheduler installiert). */
void task_set_user_aspace(int tid, uint64_t user_phys, uint64_t ttbr);

/* Task-ID (Slot-Index, INTERN) des aktuell laufenden Tasks -- recycled bei Exit. */
int sched_current_tid(void);

/* Monotone PID (nie recycled): des laufenden Tasks bzw. eines Slots. 0 = keiner/ungueltig.
 * Fuer SYS_GETPID + Diagnose-Logs -- eine PID bezeichnet eindeutig eine Prozess-Instanz. */
uint64_t sched_current_pid(void);
uint64_t task_pid(int tid);

/* Credential (uid/caps): eines Tasks setzen bzw. des aktuellen Tasks lesen.
 * Wird per proc_exec_as()/Login gesetzt; neue Tasks starten fail-closed (caps=0).
 * Capabilities werden an den Syscall-Grenzen durchgesetzt (z.B. SYS_USERADD).
 * (Echte Eltern->Kind-Vererbung folgt mit einem kuenftigen spawn-Syscall.) */
void     sched_set_cred(int tid, uint32_t uid, uint32_t caps);
uint32_t sched_current_uid(void);
uint32_t sched_current_caps(void);

/* Hook, der beim Beenden eines User-Tasks (mit user_phys != 0) gerufen wird,
 * damit proc.c den physischen Prozess-Slot freigeben kann. */
void     sched_set_exit_hook(void (*fn)(uint64_t user_phys));

/* IPC-Lock (cross-core): ipc.c nimmt ihn um sem/mutex-Ops + Block/Wake; zusaetzlich
 * muessen IRQs maskiert sein. sched_block_on gibt ihn waehrend schedule() frei. */
void    sched_ipc_lock(void);
void    sched_ipc_unlock(void);

/* IPC-Unterstuetzung (mit gehaltenem sched_ipc_lock + maskierten IRQs aufrufen):
 * aktuellen Task auf 'obj' blockieren / hoechstprioren Warter wecken / Prio lesen+setzen. */
void    sched_block_on(void *obj);
/* Wie sched_block_on, aber mit absoluter Deadline (timer_ticks-Basis). deadline == 0 ->
 * unendlich. Rueckgabe: 1 = per Timeout geweckt, 0 = per Ereignis (sched_wake_one) / Kill. */
int     sched_block_on_timeout(void *obj, uint64_t deadline);
int     sched_wake_one(void *obj);
uint8_t sched_get_prio(int tid);
void    sched_set_prio(int tid, uint8_t prio);
void    sched_reschedule(void);    /* sofortiger Reschedule (IRQs maskiert) */

/* Aus dem Timer-IRQ aufgerufen: Schlaefer wecken, Zeitscheibe verbuchen. */
void sched_tick(void);

/* Preemptionspunkt nach dem EOI im IRQ-Handler (IRQs maskiert aufrufen). */
void schedule_if_needed(void);

/* Aus dem IRQ-Handler bei einem Reschedule-IPI (SGI). Empfangs-Zaehler je Kern (Diagnose). */
void     sched_ipi_received(void);
uint32_t sched_ipi_count(uint32_t cid);

/* Anzahl Cross-Core-PI-Boost-Reschedules (Diagnose/Guardian): zaehlt, wie oft ein
 * PI-Boost einen Task auf einem FREMDEN Kern hochstufte und dessen owner-Kern anstiess. */
uint32_t sched_pi_remote_count(void);

#endif /* RPI_RTOS_SCHED_H */
