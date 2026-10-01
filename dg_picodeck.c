#include "doomgeneric.h"
#include "app_abi.h"
#include "os.h"
#include "opl_capture.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

// Doom's frame counter - declared in d_loop.c
extern int gametic;

// Doom's palette-indexed screen buffer (320x200 bytes) - declared in i_video.c
extern unsigned char *I_VideoBuffer;

// Big-endian RGB565 palette LUT - built by I_SetPalette in i_video.c
extern uint16_t rgb565_be_palette[256];

// --- Global PicoDeck state ---
const PicoCalcAPI *g_picodeck_api;
char g_app_dir[128];
static const PicoCalcAPI *s_api;
static uint32_t s_last_gametic = 0;

// longjmp target for _exit() — lets DOOM's exit()/I_Error() return to the
// OS instead of spinning in while(1).
jmp_buf g_exit_jmp;

// Doom renders to rows 60-259 (200 rows) on 320x320 screen
#define DOOM_Y_OFFSET 60
#define DOOM_Y_END 259

// --- Keys mapping ---
// doomgeneric uses its own key codes in doomkeys.h
#include "doomkeys.h"

// Doom's weapon cycling keys: the engine has them, unbound by default.
extern int key_nextweapon;
extern int key_prevweapon;
// Virtual codes no keyboard produces (NUMKEYS is 256).
#define KEY_WEAPON_NEXT 0xb0
#define KEY_WEAPON_PREV 0xb1

// Input sources feed one set of Doom keys, so a key held on both the keyboard
// and the gamepad (arrows) reports one press and one release.
typedef enum { SRC_KBD, SRC_PAD } src_t;

typedef struct {
    src_t src;
    uint32_t mask;          // BTN_* (SRC_KBD) or PAD_* (SRC_PAD)
    unsigned char doom_key;
} key_map_t;

// Always read from the keyboard (the keys no gamepad button covers).
static const key_map_t s_kbd_map[] = {
    {SRC_KBD, BTN_ENTER, KEY_ENTER},
    {SRC_KBD, BTN_ESC,   KEY_ESCAPE},
    {SRC_KBD, BTN_CTRL,  KEY_RCTRL},
    {SRC_KBD, BTN_SHIFT, KEY_RSHIFT},
    {SRC_KBD, BTN_TAB,   KEY_TAB},
    {0, 0, 0}
};

// Firmware with the gamepad (api->version >= 9): gameplay follows the player's
// bindings (Settings -> Controls). Defaults: A = F4 fire, B = F5 use, L/R =
// F2/F3 strafe, X/Y = Delete/Backspace next/previous weapon, Start = F1 menu,
// Select = Tab map, D-pad = arrows. The keys the pad owns are not read from
// the keyboard, so a rebinding moves them rather than adding to them.
static const key_map_t s_pad_map[] = {
    {SRC_PAD, PAD_UP,     KEY_UPARROW},
    {SRC_PAD, PAD_DOWN,   KEY_DOWNARROW},
    {SRC_PAD, PAD_LEFT,   KEY_LEFTARROW},
    {SRC_PAD, PAD_RIGHT,  KEY_RIGHTARROW},
    {SRC_PAD, PAD_A,      KEY_FIRE},
    {SRC_PAD, PAD_B,      KEY_USE},
    {SRC_PAD, PAD_L,      KEY_STRAFE_L},
    {SRC_PAD, PAD_R,      KEY_STRAFE_R},
    {SRC_PAD, PAD_X,      KEY_WEAPON_NEXT},
    {SRC_PAD, PAD_Y,      KEY_WEAPON_PREV},
    {SRC_PAD, PAD_START,  KEY_ESCAPE},
    {SRC_PAD, PAD_SELECT, KEY_TAB},
    {0, 0, 0}
};

// Older firmware: the fixed keys the port always used.
static const key_map_t s_legacy_map[] = {
    {SRC_KBD, BTN_UP,    KEY_UPARROW},
    {SRC_KBD, BTN_DOWN,  KEY_DOWNARROW},
    {SRC_KBD, BTN_LEFT,  KEY_LEFTARROW},
    {SRC_KBD, BTN_RIGHT, KEY_RIGHTARROW},
    {SRC_KBD, BTN_F4,    KEY_FIRE},
    {SRC_KBD, BTN_F5,    KEY_USE},
    {SRC_KBD, BTN_F1,    '1'},
    {SRC_KBD, BTN_F2,    '2'},
    {SRC_KBD, BTN_F3,    '3'},
    {0, 0, 0}
};

// Map ASCII characters to DOOM key codes where needed.
static unsigned char ascii_to_doom_key(char c) {
    switch (c) {
        case ' ':  return KEY_USE;
        default:   return (unsigned char)c;
    }
}

// --- doomgeneric implementation ---

void DG_Init() {
    // Clear both framebuffers so no stale launcher content shows through.
    // The display is double-buffered — each flush() swaps buffers, so we
    // need two clear+flush cycles to ensure both are black.
    s_api->display->clear(0x0000);
    s_api->display->flush();
    s_api->display->clear(0x0000);
    s_api->display->flush();
}

void DG_DrawFrame() {
    // Only draw and flush when Doom has produced a new frame.
    // This provides frame-skip: if rendering is slower than Doom's 35Hz tic rate,
    // we skip the unnecessary display update and let Doom catch up.
    if (gametic != s_last_gametic) {
        s_last_gametic = gametic;

        // I_VideoBuffer is NULL before I_InitGraphics allocates it, and may be
        // NULL on second launch if the zone allocator hasn't re-allocated yet.
        if (!I_VideoBuffer) {
            static bool s_logged_null = false;
            if (!s_logged_null) {
                s_api->sys->log("DOOM: I_VideoBuffer is NULL, skipping frame");
                s_logged_null = true;
            }
            return;
        }

        s_api->perf->beginFrame();

        // One-pass palette→framebuffer: convert I_VideoBuffer (palette indices)
        // directly to big-endian RGB565 in the SRAM framebuffer using the LUT.
        // This eliminates the intermediate DG_ScreenBuffer and the drawImageNN
        // byte-swap, saving ~128KB of PSRAM reads/writes per frame.
        uint16_t *fb = s_api->display->getBackBuffer();
        if (!fb) {
            s_api->perf->endFrame();
            return;
        }

        static bool s_first_frame = true;
        if (s_first_frame) {
            s_first_frame = false;
            char msg[80];
            snprintf(msg, sizeof(msg), "DOOM: fb=%p src=%p pal=%p",
                     (void*)fb, (void*)I_VideoBuffer, (void*)rgb565_be_palette);
            s_api->sys->log(msg);
        }

        const unsigned char *src = I_VideoBuffer;
        const uint16_t *pal = rgb565_be_palette;

        // PHASE-0 DIAG: breadcrumbs around the render + flush steps so we can
        // pinpoint which stage hangs on the first frame.
        static int s_diag_calls = 0;
        bool diag = (s_diag_calls < 3);
        if (diag) s_api->sys->log("DOOM: render begin");

        for (int y = 0; y < 200; y++) {
            uint16_t *dst = &fb[(y + DOOM_Y_OFFSET) * 320];
            const unsigned char *row = &src[y * 320];
            for (int x = 0; x < 320; x += 4) {
                dst[x]     = pal[row[x]];
                dst[x + 1] = pal[row[x + 1]];
                dst[x + 2] = pal[row[x + 2]];
                dst[x + 3] = pal[row[x + 3]];
            }
        }

        if (diag) s_api->sys->log("DOOM: render done");

        // Flush only the active region (rows 59-260 with margin) instead of full 320x320.
        // This reduces DMA transfer by ~38% (64K pixels vs 102K pixels).
        if (diag) s_api->sys->log("DOOM: flush begin");
        s_api->display->flushRegion(DOOM_Y_OFFSET - 1, DOOM_Y_OFFSET + 200);
        if (diag) s_api->sys->log("DOOM: flush done");

        s_api->perf->endFrame();

        if (diag) { s_diag_calls++; s_api->sys->log("DOOM: frame done"); }
    }
}

void DG_SleepMs(uint32_t ms) {
    // Feed the watchdog during any spin-wait (e.g. TryRunTics).
    s_api->sys->poll();
}

uint32_t DG_GetTicksMs() {
    uint32_t now = s_api->sys->getTimeMs();
    // Feed the watchdog during the long init phase (WAD loading, hash tables,
    // subsystem init) which runs entirely inside doomgeneric_Create() before
    // the main loop gets a chance to call poll(). Throttled to once per 500ms
    // to avoid hammering kbd_poll() on every timing query.
    static uint32_t s_last_poll_ms = 0;
    if (now - s_last_poll_ms >= 500) {
        s_last_poll_ms = now;
        s_api->sys->poll();
    }
    return now;
}

static bool use_gamepad(void) {
    return s_api->version >= 9 && s_api->gamepad;
}

static uint32_t held_doom_keys(unsigned char *keys, int *nkeys) {
    uint32_t kbd = s_api->input->getButtons();
    uint32_t pad = use_gamepad() ? s_api->gamepad->getButtons() : 0;
    const key_map_t *maps[3] = {
        s_kbd_map, use_gamepad() ? s_pad_map : s_legacy_map, NULL};
    uint32_t held = 0;
    *nkeys = 0;
    for (int m = 0; maps[m]; m++) {
        for (const key_map_t *e = maps[m]; e->mask; e++) {
            int idx = -1;
            for (int i = 0; i < *nkeys; i++)
                if (keys[i] == e->doom_key) { idx = i; break; }
            if (idx < 0) idx = (*nkeys)++, keys[idx] = e->doom_key;
            uint32_t cur = (e->src == SRC_PAD) ? pad : kbd;
            if (cur & e->mask) held |= 1u << idx;
        }
    }
    return held;
}

int DG_GetKey(int* pressed, unsigned char* key) {
    // Doom wants one event per call: report the first key whose state differs
    // from what Doom was last told. The key list is rebuilt in a fixed order
    // each call, so the bit positions are stable.
    static uint32_t last_held = 0;
    unsigned char keys[32];
    int nkeys;
    uint32_t held = held_doom_keys(keys, &nkeys);
    uint32_t changed = held ^ last_held;
    if (changed) {
        for (int i = 0; i < nkeys; i++) {
            if (changed & (1u << i)) {
                *pressed = (held >> i) & 1;
                *key = keys[i];
                last_held ^= 1u << i;
                return 1;
            }
        }
    }

    // Character keys: getChar() returns the last character from kbd_poll().
    // It does NOT consume the value, so we must track the previous character
    // to avoid reporting the same key every time DG_GetKey is called within
    // a single tick (which would cause an infinite loop in DOOM's input
    // polling).  We generate a press when a new character appears and a
    // release when it disappears (next poll cycle clears it to 0).
    static char last_char = 0;
    char c = s_api->input->getChar();
    if (c && c != last_char) {
        last_char = c;
        *pressed = 1;
        *key = ascii_to_doom_key(c);
        return 1;
    } else if (!c && last_char) {
        *pressed = 0;
        *key = ascii_to_doom_key(last_char);
        last_char = 0;
        return 1;
    }

    return 0;
}

void DG_SetWindowTitle(const char * title) {
    // No window title in PicoDeck
}

// --- PicoDeck Entry Point ---

void picodeck_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    s_api = api;
    g_picodeck_api = api;
    strncpy(g_app_dir, app_dir, sizeof(g_app_dir) - 1);
    g_app_dir[sizeof(g_app_dir) - 1] = '\0';

    api->sys->log("DOOM: Starting...");

    // If DOOM calls exit() (e.g. from I_Error), longjmp back here instead
    // of spinning forever in _exit()'s while(1).
    int exit_code = setjmp(g_exit_jmp);
    if (exit_code != 0) {
        opl_capture_stop();
        api->sys->setAudioCallback(NULL);  // stop Core 1 mixing
        api->sys->log("DOOM: exit() called, returning to launcher");
        return;
    }

    // Build path to doom1.wad in app directory
    static char wad_path[256];
    snprintf(wad_path, sizeof(wad_path), "%s/doom1.wad", app_dir);

    // Pass -gfxmode rgb565 and -iwad pointing to the app directory.
    // -nomusic: the Nuked-OPL3 synth sustains only ~6.5k samples/s from
    // 50 MHz serial-mode PSRAM (11025 needed) — music playback starves the
    // Core 1 mixer into constant underruns and drags SFX down with it.
    // Re-enable once the QMI runs the PSRAM in quad mode (~4x bandwidth)
    // or a lighter OPL core is used.  SFX mixes at full rate without it.
    char* argv[] = {"doom", "-gfxmode", "rgb565", "-nomusic",
                    "-iwad", wad_path, NULL};

    // Initialize DOOM (runs one tick internally, then returns)
    doomgeneric_Create(5, argv);

    // Weapon cycling is unbound in Doom; the gamepad's X/Y drive it. Set after
    // the config load so a saved default.cfg cannot unbind them.
    key_nextweapon = KEY_WEAPON_NEXT;
    key_prevweapon = KEY_WEAPON_PREV;

    // Main game loop — doomgeneric expects the platform to drive ticks
    while (!api->sys->shouldExit()) {
        api->sys->poll();

        doomgeneric_Tick();
    }

    opl_capture_stop();
    api->sys->setAudioCallback(NULL);  // stop Core 1 mixing
    api->sys->log("DOOM: Exiting...");
}
