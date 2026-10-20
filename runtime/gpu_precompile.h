/* The start-up's shader screen: before the game starts, the shaders of its index that the disk
 * cache lacks are compiled by tools/shaderaot.c processes behind a progress screen
 * (gpu_precompile.c). */
#pragma once
#include <stdio.h>

/* On the start-up thread, before the game's: returns when the cache is complete, Esc was
 * pressed, or there is nothing to do. SSBU_PRECOMPILE=0 turns it off; Linux only. */
void precompile_run(void);

/* The window's thread: while the screen is up (precompile_active), Esc sets precompile_skip. */
extern volatile int precompile_active, precompile_skip;

/* (the parts the tests drive) The keys of keys_path ("vs <hash>" / "ps <hash> <vs outputs>", as
 * shaderaot --dry prints them) whose cache files are not in cache_dir, written to missing (one
 * per line, the same form): how many, or -1 if keys_path cannot be read. */
long precompile_missing(const char *keys_path, const char *cache_dir, FILE *missing);
/* A "done <compiled> <cached> <left out or failed>" line of shaderaot --progress, into counts: 1, or 0 if it is not one. */
int precompile_done_line(const char *line, unsigned long counts[3]);
