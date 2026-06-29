/*
 * 3ds_compat.c — newlib compatibility for the port's freestanding shims.
 * The shims declare `extern void* stderr` and call fprintf(stderr, ...), but
 * newlib makes `stderr` a macro (no global symbol). Provide a real `stderr`
 * symbol, routed to fd 1 so shim debug output lands on the bottom-screen
 * console (consoleInit GFX_BOTTOM) — useful for on-hardware debugging.
 */
#include <stdio.h>
#undef stderr

FILE* stderr;

/* Called explicitly from main() AFTER consoleInit — NOT a crt0 constructor
 * (running fdopen during early startup risked crashing before main). */
void PortCompat_InitStreams(void) {
    stderr = fdopen(1, "w");        /* fd 1 -> 3DS console */
    if (stderr) setvbuf(stderr, NULL, _IONBF, 0);
}
