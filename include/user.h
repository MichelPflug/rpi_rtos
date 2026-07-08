/*
 * include/user.h  --  Benutzerverwaltung (Konten, Auth, Capabilities)
 *
 * Konten liegen persistent auf der System-Partition (hdd0:USERS.DB) und werden
 * nur ueber den privilegierten VFS-Schreibpfad veraendert. Passwoerter werden
 * als PBKDF2-HMAC-SHA256 (pro Konto zufaelliger Salt) gespeichert -- nie Klartext.
 */
#ifndef RPI_RTOS_USER_H
#define RPI_RTOS_USER_H

#include <stdint.h>

#define USER_NAME_MAX  31
#define USER_CAP_ADMIN 0x1u          /* darf Konten anlegen/loeschen */
#define USER_CAP_GUI   0x2u          /* darf den Framebuffer bespielen (GUI-Grafik-Bruecke, T2.1):
                                      * nur ein Prozess mit dieser Cap bekommt den Backbuffer gemappt
                                      * und darf SYS_GUI_INFO/SYS_GUI_FLUSH nutzen -> kein Cross-App-
                                      * Screen-Leak/-Spoofing durch beliebige EL0-Prozesse. */
#define USER_CAP_ALL   0xFFFFFFFFu

/* Laedt die DB von hdd0; legt bei leerer/fehlender DB ein Default-Konto
 * 'admin' (Passwort 'admin') an und persistiert es. 0 = ok. */
int  user_init(void);

/* Prueft Name+Passwort. Liefert die uid (>= 0) bei Erfolg, sonst -1. */
int  user_authenticate(const char *name, const char *password);

/* Wie authenticate, liefert zusaetzlich uid + Capabilities (fuer den cred-Block
 * eines Login-Prozesses). 0 = ok, -1 = Login fehlgeschlagen. */
int  user_login(const char *name, const char *password,
                uint32_t *uid_out, uint32_t *caps_out);

/* Legt ein Konto an / loescht eines. actor_caps muss USER_CAP_ADMIN enthalten.
 * 0 = ok, -1 = Fehler (keine Rechte / existiert / voll / ungueltig). */
int  user_add(const char *name, const char *password, uint32_t caps, uint32_t actor_caps);
int  user_delete(const char *name, uint32_t actor_caps);

/* Aendert das Passwort des Kontos mit der gegebenen uid (neues Salt + PBKDF2).
 * Jeder darf nur sein eigenes Passwort aendern (Aufrufer prueft uid). Loescht das
 * must_change-Flag. 0 = ok, -1. */
int  user_change_password(uint32_t uid, const char *newpw);

/* 1, wenn das Konto mit dieser uid beim Login einen Passwortwechsel erzwingen muss
 * (Default-Konto bzw. vom Admin angelegtes Erstpasswort). */
int  user_must_change(uint32_t uid);

void user_list(void);
int  user_count(void);

/* Interaktiver Login ueber die serielle Konsole (Name/Passwort, 3 Versuche).
 * Bei Erfolg 0 und uid/caps des Benutzers in den Out-Parametern, sonst -1.
 * Blockiert auf Serial-Eingabe -> nur in Builds mit -DINTERACTIVE_LOGIN aufrufen. */
int  login_console(uint32_t *uid_out, uint32_t *caps_out);

#ifdef RTOS_SELFTEST
/* Selbsttest der crash-sicheren DB-Wiederherstellung (korruptes Primaer -> Backup-Fallback).
 * 1 = Recovery ok, 0 = fehlgeschlagen. Nutzt eigene Test-Dateien, laesst den echten
 * DB-Zustand unangetastet. Nur im Selbsttest-Build vorhanden. */
int  user_selftest_db_recovery(void);
#endif

#endif /* RPI_RTOS_USER_H */
