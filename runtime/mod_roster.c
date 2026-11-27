/* The Mods page's grid of fighters: see mod_roster.h. */
#include "mod_roster.h"

#include <string.h>

static const RosterTile TILES[ROSTER_OTHER] = {
    {{"mario"}, "mario"}, {{"donkey"}, "donkey"}, {{"link"}, "link"}, {{"samus"}, "samus"}, {{"samusd"}, "samusd"},
    {{"yoshi"}, "yoshi"}, {{"kirby"}, "kirby"}, {{"fox"}, "fox"}, {{"pikachu"}, "pikachu"}, {{"luigi"}, "luigi"},
    {{"ness"}, "ness"}, {{"captain"}, "captain"}, {{"purin"}, "purin"}, {{"peach"}, "peach"}, {{"daisy"}, "daisy"},
    {{"koopa"}, "koopa"}, {{"ice_climber", "popo", "nana"}, "ice_climber"}, {{"sheik"}, "sheik"}, {{"zelda"}, "zelda"},
    {{"mariod"}, "mariod"}, {{"pichu"}, "pichu"}, {{"falco"}, "falco"}, {{"marth"}, "marth"}, {{"lucina"}, "lucina"},
    {{"younglink"}, "younglink"}, {{"ganon"}, "ganon"}, {{"mewtwo"}, "mewtwo"}, {{"roy"}, "roy"}, {{"chrom"}, "chrom"},
    {{"gamewatch"}, "gamewatch"}, {{"metaknight"}, "metaknight"}, {{"pit"}, "pit"}, {{"pitb"}, "pitb"},
    {{"szerosuit"}, "szerosuit"}, {{"wario"}, "wario"}, {{"snake"}, "snake"}, {{"ike"}, "ike"},
    {{"ptrainer", "pzenigame", "pfushigisou", "plizardon"}, "ptrainer"}, {{"diddy"}, "diddy"}, {{"lucas"}, "lucas"},
    {{"sonic"}, "sonic"}, {{"dedede"}, "dedede"}, {{"pikmin"}, "pikmin"}, {{"lucario"}, "lucario"}, {{"robot"}, "robot"},
    {{"toonlink"}, "toonlink"}, {{"wolf"}, "wolf"}, {{"murabito"}, "murabito"}, {{"rockman"}, "rockman"},
    {{"wiifit"}, "wiifit"}, {{"rosetta"}, "rosetta"}, {{"littlemac"}, "littlemac"}, {{"gekkouga"}, "gekkouga"},
    {{"palutena"}, "palutena"}, {{"pacman"}, "pacman"}, {{"reflet"}, "reflet"}, {{"shulk"}, "shulk"},
    {{"koopajr"}, "koopajr"}, {{"duckhunt"}, "duckhunt"}, {{"ryu"}, "ryu"}, {{"ken"}, "ken"}, {{"cloud"}, "cloud"},
    {{"kamui"}, "kamui"}, {{"bayonetta"}, "bayonetta"}, {{"inkling"}, "inkling"}, {{"ridley"}, "ridley"},
    {{"simon"}, "simon"}, {{"richter"}, "richter"}, {{"krool"}, "krool"}, {{"shizue"}, "shizue"},
    {{"gaogaen"}, "gaogaen"},
    /* the DLC (their pictures in ui/replace_patch/ alone) */
    {{"packun"}, "packun"}, {{"jack"}, "jack"}, {{"brave"}, "brave"}, {{"buddy"}, "buddy"}, {{"dolly"}, "dolly"},
    {{"master"}, "master"}, {{"tantan"}, "tantan"}, {{"pickel"}, "pickel"}, {{"edge"}, "edge"},
    {{"element", "eflame", "elight"}, "eflame_first"}, {{"demon"}, "demon"}, {{"trail"}, "trail"},
    /* the Miis: a tile each (a folder and eight colors each), not the select screen's one */
    {{"miifighter"}, "miifighter"}, {{"miiswordsman"}, "miiswordsman"}, {{"miigunner"}, "miigunner"},
};

const RosterTile *mod_roster(int tile) { return tile >= 0 && tile < ROSTER_OTHER ? &TILES[tile] : NULL; }

int mod_roster_tile(const char *f) {
    if (!f || !*f) return ROSTER_OTHER;
    for (int t = 0; t < ROSTER_OTHER; t++)
        for (int k = 0; k < ROSTER_NAMES && TILES[t].names[k]; k++)
            if (!strcmp(TILES[t].names[k], f)) return t;
    return ROSTER_OTHER;
}

const char *mod_roster_name(int tile) {
    const RosterTile *r = mod_roster(tile);
    return r ? mod_fighter_name(r->names[0]) : "Other";
}

int mod_roster_of(const ModInfo *x) {
    int tile = -1;
    for (int i = 0; i < x->npaths; i++) {
        char f[64];
        if (!mod_path_fighter(x->paths[i], f, sizeof f)) continue;
        int t = mod_roster_tile(f);
        if (t == ROSTER_OTHER) continue;   /* (fighter/common/, Giga Bowser's koopag...: no one's tile) */
        if (tile >= 0 && t != tile) return ROSTER_OTHER;
        tile = t;
    }
    return tile >= 0 ? tile : ROSTER_OTHER;
}
