#ifndef SEGMENT_SYMBOLS_H
#define SEGMENT_SYMBOLS_H

/* PORT OVERRIDE for 3DS: the original declares segment markers as extern
 * symbols (extern u8 _xSegmentStart[]). On the relocatable .3dsx those become
 * absolute out-of-image relocations that 3dsxtool rejects. Instead we bake the
 * N64 addresses as compile-time pointer constants (3ds_segment_consts.h) and
 * make the DECLARE_* macros no-ops. Values are used for size arithmetic and as
 * opaque overlay tokens (never dereferenced — identity-overlay scheme). */

#include "ultra64/ultratypes.h"
#include "versions.h"
#include "3ds_segment_consts.h"

#define DECLARE_SEGMENT(name)
#define DECLARE_ROM_SEGMENT(name)
#define DECLARE_BSS_SEGMENT(name)
#define DECLARE_OVERLAY_SEGMENT(name)

#endif
