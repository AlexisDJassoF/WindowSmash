/* The Mods page's grid of fighters (mod_roster.c): a tile per fighter of Smash Ultimate's character
 * select, in its order (ui_chara_db.prc's disp_order, 13.0.4: the echoes beside their originals, the
 * DLC after Incineroar, the three Miis last), then Other - mods of no fighter or of several, and the
 * rooms' mods. A tile holds every internal name of its fighter (Pokemon Trainer and its three
 * Pokemon, the Ice Climbers, Pyra and Mythra...); the first one is the fighter's, and portrait names
 * its select screen picture (ui/replace[_patch]/chara/chara_7/chara_7_<portrait>_00.bntx). */
#pragma once
#include "mod_scan.h"

#define ROSTER_NAMES 5
typedef struct RosterTile {
    const char *names[ROSTER_NAMES];   /* the internal names (NULL after the last) */
    const char *portrait;              /* chara_7_<portrait>_<color>.bntx */
} RosterTile;

#define ROSTER_TILES 87               /* the fighters, then Other */
#define ROSTER_OTHER (ROSTER_TILES - 1)

const RosterTile *mod_roster(int tile);            /* NULL for Other and past it */
int mod_roster_tile(const char *fighter);           /* the tile holding that internal name; ROSTER_OTHER none */
const char *mod_roster_name(int tile);              /* "Pokemon Trainer", "Other" */
/* The tile a mod is shown in: its fighters' (every path of a fighter in the same tile; fighter/common/
 * and the like, of no tile, not counted), else Other */
int mod_roster_of(const ModInfo *x);
