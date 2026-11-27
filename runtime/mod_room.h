/* A room's mods, for who joins (openspec change netplay-mod-sync, room-mod-download): the host asked
 * for its manifest, compared with what is kept of it (mod_cache.h), the dialog - each mod and what
 * it needs, the mode (mirror / overlay), Download and apply, Play without them (chosen when it opens)
 * and Menu -, the download with its progress, and what the game then loads.
 *
 *   A private room: Start on the start-up menu with Join chosen joins the room's relay, asks, and -
 *   when there is something to agree to - shows the dialog; the mods accepted are fetched and used in
 *   this same start (SSBU_ROOM_MODS, read when the game opens data.arc: hle_fs.c).
 *   The online lobby: the game is running; entering a room asks its host (net_relay.c calls
 *   mod_room_joined), and the dialog comes over the game. The download goes on behind the game,
 *   then Restart and rejoin (the program started again with them, on the Network page) or Later
 *   (kept for the next time).
 * netplay.ini's room_mods = never asks nothing; always fetches without asking, in room_mods_mode.
 * Nothing here draws or reads a window: gpu_menu.c does (mod_room_view, mod_room_event); the tests
 * drive it as it is (tests/test_mod_room.py). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* (net_relay.c's thread) A lobby's room just entered: its host, at that address. */
void mod_room_joined(uint32_t host, int lobby);

/* Where things are and what netplay.ini says, before anything else (the menu, the game's start) */
typedef struct RoomPrefs {
    int room_mods;             /* ROOM_MODS_* (net_menu.h) */
    int mode;                  /* ROOM_MIRROR / ROOM_OVERLAY */
    char mods_root[512];
    char hash_cache[512];      /* the player's own files' hashes (cache/mods/hashes.txt) */
} RoomPrefs;
void mod_room_setup(const RoomPrefs *p);
/* The start's tidying of mods/.netplay (once a process): hosts not joined for 30 days forgotten */
void mod_room_tidy(void);

/* A private room, from the start-up menu: ask the host now (the room's broadcast address) */
void mod_room_begin(uint32_t host, int lobby);
void mod_room_tick(void);      /* the caller's thread, often: the answers, the download */

enum { RM_IDLE, RM_ASKING, RM_DIALOG, RM_FETCHING, RM_AFTER, RM_READY };
int mod_room_stage(void);      /* RM_*: RM_READY when the start-up menu may go on to the game */
int mod_room_busy(void);       /* (the lobby) something to show over the game now */

enum { RM_EV_NONE, RM_EV_PLAY, RM_EV_MENU, RM_EV_RESTART };
/* MENU_UP/DOWN/LEFT/RIGHT/ACCEPT/BACK (gpu_menu.h): RM_EV_PLAY - the start-up menu goes on to the game
 * (the lobby: the dialog closes) -, RM_EV_MENU - back to the menu: a private room's Network page, or
 * the lobby left and the program started again on the menu -, RM_EV_RESTART (the lobby, the mods
 * here: the program started again with them) */
int mod_room_event(int ev);
int mod_room_click(int button);   /* a button clicked (0..), or -1 the mode */

#define ROOM_LINES 48
typedef struct RoomView {
    int show;                  /* a panel to draw */
    char title[160];
    int nlines;
    char lines[ROOM_LINES][200];   /* the first character the kind: '+' new, '~' changed, '>' moved, '=' kept, '-' the host
                                    * dropped it, '!' a warning, 'x' refused, 'i' a note */
    int mode_row;              /* the mode's row is shown */
    char mode[32], mode_text[160];
    int nbuttons;
    char buttons[3][40];
    int sel;                   /* the button selected, or -1 the mode's row */
    int progress;              /* 0..1000 while downloading, else -1 */
    char status[200];          /* under the bar */
} RoomView;
void mod_room_view(RoomView *v);

/* The text the start-up menu's footer or the overlay shows once (what was applied by itself), "" none */
const char *mod_room_note(void);
