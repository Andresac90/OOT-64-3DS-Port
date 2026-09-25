/* layout_probe.c — compiled with the 3DS game flags + -g ONLY to emit DWARF describing the game's
 * structs; tools/statediff/layout.py reads it to know every field's offset/size/type.
 * Never linked. The 3DS and N64 layouts are the same (both 32-bit, same alignment); layout.py
 * checks the sizes against the decomp's "size = 0x..." annotations to prove that. */
#include "ultra64.h"
#include "play_state.h"
#include "save.h"
#include "environment.h"
#include "actor.h"
#include "player.h"

PlayState probe_PlayState;
SaveContext probe_SaveContext;
Actor probe_Actor;
Player probe_Player;
EnvironmentContext probe_EnvironmentContext;
