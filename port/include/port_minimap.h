#ifndef PORT_MINIMAP_H
#define PORT_MINIMAP_H

/* PORT: the minimap as data, so the 3DS can draw it on the touch screen instead of
 * over the top screen. Minimap_Draw fills this each frame it would have drawn the map; positions are
 * in N64 screen pixels (320x240), exactly where the N64 would have drawn them. */

#define PORT_MINIMAP_MAX_MARKS 16

typedef struct {
    unsigned int serial; /* bumped on every fill; unchanged = map not drawn this frame */
    const void* tex;     /* 4-bit texture: I4 (dungeon) or IA4 (overworld) */
    unsigned char fmt;   /* PORT_MINIMAP_I4 / PORT_MINIMAP_IA4 */
    unsigned char r, g, b;
    short w, h;
    short x, y; /* top-left on the N64 screen */
    unsigned char compass;
    float playerX, playerY; /* arrow centers on the N64 screen */
    short playerYaw;        /* shape.rot.y */
    float startX, startY;
    short startYaw;
    unsigned char numIcons; /* dungeon-entrance icons (8x8) */
    short iconX[2], iconY[2];
    unsigned char numMarks; /* dungeon chests / boss */
    unsigned char markType[PORT_MINIMAP_MAX_MARKS];
    short markX[PORT_MINIMAP_MAX_MARKS], markY[PORT_MINIMAP_MAX_MARKS];
} PortMinimap;

#define PORT_MINIMAP_I4 1
#define PORT_MINIMAP_IA4 2

extern PortMinimap gPortMinimap;
extern int gPortMinimapOnBottom; /* 1 = the touch panel shows the map; the top screen does not */

extern const unsigned char* gPortHudIconSeg; /* interfaceCtx->iconItemSegment: B, C-left/down/right icons */
extern unsigned int gPortHudSerial;           /* bumped by every Interface_Draw */
extern int gPortHudKeys;                      /* small keys shown by the HUD, -1 = none */
extern int gPortHudNavi;                      /* Navi wants to talk (the HUD's "Navi" C-up prompt) */
extern int gPortHudTop;                       /* 1 = the N64 HUD on the top screen (default), 0 = hidden */
extern volatile int gPortTouchOcarina, gPortTouchBoots; /* touch requests, consumed by z_player.c */
extern volatile int gPortTouchPage; /* pause page for the next START (-1 = the game's own), z_kaleido_setup.c */

/* PORT (2026-10-07): dungeon floors for the touch screen (z_map_exp.c Map_ExportFloors): the floor list the pause
 * map page shows, and the map of one floor on request - the pause page's picture (two 48x85 CI4 halves side by
 * side, game memory layout: byte k at k ^ 7) with its palette (visited rooms, the outline with the Map) */
#define PORT_FLOOR_MAX 8
typedef struct {
    unsigned int serial;   /* bumped by every update that fills it */
    int numFloors;         /* 0 = no floor list here (not a dungeon) */
    unsigned char slot[PORT_FLOOR_MAX];  /* the game's floor slots shown, top floor first */
    unsigned char label[PORT_FLOOR_MAX]; /* F_* (map.h): 1..8 = 8F..1F, 9.. = B1.. */
    int curSlot;           /* Link's floor */
    int mapSlot;           /* the floor whose map is in tex/pal, -1 = none yet */
    const unsigned char* tex;
    unsigned short pal[16]; /* RGBA5551 */
    int curRoomIndex;       /* palette index of Link's room on mapSlot's map, -1 = not on it */
} PortFloorMap;
extern PortFloorMap gPortFloorMap;
extern volatile int gPortFloorReq; /* the floor slot the touch screen wants the map of, -1 = none */

/* everything else the panel shows, read from the save by Port_GetHudInfo (ultra_shims.c) */
typedef struct {
    int valid; /* a save is loaded and the game is in normal play mode */
    int rupees, keys;
    int health, healthCapacity; /* 16 per heart */
    int magic, magicCapacity;
    int boots;      /* EQUIP_VALUE_BOOTS_* (1 kokiri, 2 iron, 3 hover) */
    int ocarina;    /* item id in the ocarina slot */
    unsigned char cItem[3], cDisabled[3];
    short cAmmo[3]; /* -1 = no counter */
} PortHudInfo;
void Port_GetHudInfo(PortHudInfo* h);

#endif
