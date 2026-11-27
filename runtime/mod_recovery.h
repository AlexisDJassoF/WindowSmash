/* The recovery from a mod that breaks the game (mod_recovery.c; openspec mod-support, mod-recovery).
 *
 * With mods enabled, cache/mods/session.txt says how the session is going: "starting" when the game is
 * about to open data.arc (the mods' print, and which ones are new or changed since the last start that
 * worked), "running" once frames have been presented for 20 s (and cache/mods/last_good.txt is then the
 * mods of now), "crashed" or "frozen" when it fails - written by the fault handlers, rt_fatal and the
 * freeze watchdog, with the last mod files the game read. A normal close deletes it. At the next start,
 * a session.txt left behind is a failed one: the start-up menu opens on the Mods page with what
 * happened, the suspect (the mod of the last file read; else the new ones; else none: probably not the
 * mods) and what to do - disable it, go back to the last mods that worked, start without mods this
 * once, or ignore. Two failed sessions in a row with the same mods and no choice made: the game starts
 * without mods (they stay enabled).
 *
 * The parts that decide are plain functions on a folder (tests/test_mod_recovery.py drives them); the
 * runtime calls the hooks at the bottom. */
#pragma once
#include <stdint.h>

#define REC_NEWS 16
enum { REC_NONE, REC_UNCLEAN, REC_CRASH, REC_FATAL, REC_FROZEN };   /* how the last session ended */

typedef struct Recovery {
    int failed;                   /* REC_*: REC_NONE, the last session did not fail */
    int was_running;              /* it had started (20 s of frames): a fight's load, say */
    char suspect[128];            /* the suspect mod's folder name, "" none */
    char suspect_path[256];       /* the last of its files read (data.arc's path), "" none */
    int suspect_new;              /* the suspect is the new mods (nothing of a mod was read) */
    char news[REC_NEWS][128];     /* the mods new or changed since the last start that worked */
    int nnews;
    char print[17];               /* the failed session's mods (a hash), hex */
    int fails;                    /* failed sessions in a row with those mods, it included */
    int no_mods;                  /* this start goes without mods: two failures in a row, nothing chosen */
    int safe;                     /* the safe mode asked (Shift, L+R): the menu with "without mods" chosen */
} Recovery;

/* The mods of a session: each one's folder name and a hash of its files (paths, sizes, dates) */
typedef struct RecoveryMod { char key[128]; uint64_t hash; } RecoveryMod;

/* <dir>/session.txt read: 1 if the last session failed (*r filled), 0 none or a clean one. The file
 * is deleted (what it said is in *r now). */
int mod_recovery_read(const char *dir, Recovery *r);
/* The new session's file: "starting", its mods' print, the new ones (against <dir>/last_good.txt).
 * prev: what mod_recovery_read found (NULL: nothing). 1 if this start must go without mods (prev failed
 * twice in a row with the same mods, prev->no_mods not cleared by a choice), and then nothing is written. */
int mod_recovery_begin(const char *dir, const RecoveryMod *mods, int n, const Recovery *prev);
/* The session got going: "running", and last_good.txt its mods */
void mod_recovery_running(const char *dir);
/* The session failed (REC_CRASH, REC_FATAL, REC_FROZEN): written with the files read last (a fault
 * handler's: no allocation, only open/write/close) */
void mod_recovery_failed(int kind);
void mod_recovery_end(const char *dir);   /* a normal close: session.txt deleted */
/* The mods last_good.txt names (a folder name a line), into keys: how many */
int mod_recovery_last_good(const char *dir, char keys[][128], int max);

/* A mod file the game read (mod_arc_read's): kept with the 7 before it, without a lock */
void mod_recovery_read_file(const char *mod, const char *path);

/* ---- the runtime's ---------------------------------------------------------------------------- */
/* cache/mods (next to mods/): into out */
const char *mod_recovery_dir(char *out, unsigned n);
/* The last session, read once per process (the start-up menu, or the first data.arc with mods) */
Recovery *mod_recovery_state(void);
/* The menu's choice was made (any of them): the two-failures rule is not applied this start */
void mod_recovery_chosen(void);
/* This start goes without mods (the recovery's choice, the two-failures rule): for F2 */
extern volatile int mod_recovery_mods_off;
/* F2's line ("" none): started without mods by the recovery (the players of a room may expect this
 * player's skins), else the mods loaded that can desync online play (mod_gameplay_note, hle_fs.c's) */
void mod_recovery_note(char *out, unsigned n);
extern char mod_gameplay_note[200];
/* hle_fs.c, the first data.arc with mods: the session begun (0 = start without mods) */
struct ModArcFile;
int mod_recovery_session(const struct ModArcFile *files, int n);
/* watchdog: frames presented for a while (running), or none for 30 s (frozen), or again after it */
void mod_recovery_frames(int presenting_for_s);
void mod_recovery_frozen(void);
void mod_recovery_thawed(void);
void mod_recovery_close(void);   /* the window closed normally (or the program starts itself again) */
