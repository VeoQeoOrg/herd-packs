#include "doomkeys.h"
#include "m_argv.h"
#include "m_controls.h"
#include "doomgeneric.h"
#include "d_event.h"
#include "doomstat.h"
#include "i_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/cervus.h>

#define EV_KEY     0x01
#define EV_REL     0x02
#define REL_X      0x00
#define REL_WHEEL  0x08
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

typedef struct {
    uint64_t sec;
    uint64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed)) evdev_event_t;

#define KQ_SIZE 128

static cervus_fb_info_t  g_fb;
static uint32_t         *g_line;
static unsigned          g_scale;
static unsigned          g_ox, g_oy;
static int               g_windowed;
static int               g_kbd = -1;
static int               g_mouse = -1;
static struct termios    g_tio;
static int               g_have_tio;
static int               g_acquired;
static int               g_buttons;
static int               g_grabbed;
static int               g_focused = 1;
static int               g_hint_shown;

static unsigned short    g_queue[KQ_SIZE];
static unsigned          g_qhead, g_qtail;
static unsigned char     g_sent[256];

static const char HELP[] =
    "Doom controls\n"
    "  W A S D / arrows   move and strafe (arrows left/right turn)\n"
    "  mouse              turn;  left click or Ctrl fires\n"
    "  E / Space / right  use, open doors\n"
    "  Shift              run\n"
    "  1-7 / wheel        weapons\n"
    "  Tab                map\n"
    "  Esc                menu (Enter selects, arrows move)\n"
    "  F10                quit\n";

static unsigned char linux_to_doom(unsigned code)
{
    static const unsigned char letters[] = {
        [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't', [21] = 'y',
        [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p', [30] = 'a', [31] = 's',
        [32] = 'd', [33] = 'f', [34] = 'g', [35] = 'h', [36] = 'j', [37] = 'k',
        [38] = 'l', [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
        [49] = 'n', [50] = 'm',
        [2] = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5', [7] = '6',
        [8] = '7', [9] = '8', [10] = '9', [11] = '0',
        [26] = '[', [27] = ']', [39] = ';', [40] = '\'', [41] = '`', [43] = '\\',
        [51] = ',', [52] = '.', [53] = '/',
    };
    switch (code) {
        case 1:   return KEY_ESCAPE;
        case 12:  return KEY_MINUS;
        case 13:  return KEY_EQUALS;
        case 14:  return KEY_BACKSPACE;
        case 15:  return KEY_TAB;
        case 28:  return KEY_ENTER;
        case 96:  return KEY_ENTER;
        case 29:  return KEY_FIRE;
        case 97:  return KEY_FIRE;
        case 42:  return KEY_RSHIFT;
        case 54:  return KEY_RSHIFT;
        case 56:  return KEY_RALT;
        case 100: return KEY_RALT;
        case 57:  return KEY_USE;
        case 59:  return KEY_F1;
        case 60:  return KEY_F2;
        case 61:  return KEY_F3;
        case 62:  return KEY_F4;
        case 63:  return KEY_F5;
        case 64:  return KEY_F6;
        case 65:  return KEY_F7;
        case 66:  return KEY_F8;
        case 67:  return KEY_F9;
        case 68:  return KEY_F10;
        case 87:  return KEY_F11;
        case 88:  return KEY_F12;
        case 103: return KEY_UPARROW;
        case 108: return KEY_DOWNARROW;
        case 105: return KEY_LEFTARROW;
        case 106: return KEY_RIGHTARROW;
        case 102: return KEY_HOME;
        case 107: return KEY_END;
        case 104: return KEY_PGUP;
        case 109: return KEY_PGDN;
        case 110: return KEY_INS;
        case 111: return KEY_DEL;
        case 119: return KEY_PAUSE;
        default:  break;
    }
    if (code < sizeof letters) return letters[code];
    return 0;
}

static int in_game(void)
{
    return gamestate == GS_LEVEL && !menuactive;
}

static void queue_key(int pressed, unsigned char key)
{
    unsigned next = (g_qhead + 1) % KQ_SIZE;
    if (next == g_qtail) return;
    g_queue[g_qhead] = (unsigned short)((pressed ? 0x100 : 0) | key);
    g_qhead = next;
}

static void key_event(unsigned code, int pressed)
{
    if (code >= 256) return;
    if (!pressed) {
        if (g_sent[code]) queue_key(0, g_sent[code]);
        g_sent[code] = 0;
        return;
    }
    if (g_sent[code]) return;
    unsigned char k = linux_to_doom(code);
    if (in_game()) {
        switch (code) {
            case 17: k = KEY_UPARROW;   break;
            case 31: k = KEY_DOWNARROW; break;
            case 30: k = KEY_STRAFE_L;  break;
            case 32: k = KEY_STRAFE_R;  break;
            case 18: k = KEY_USE;       break;
            default: break;
        }
    } else if (code == 57) {
        k = ' ';
    }
    if (!k) return;
    g_sent[code] = k;
    queue_key(1, k);
}

static void post_mouse(int dx, int extra)
{
    event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = ev_mouse;
    ev.data1 = g_buttons | extra;
    ev.data2 = dx;
    ev.data3 = 0;
    D_PostEvent(&ev);
}

static void button_event(unsigned code, int pressed)
{
    int bit = code == BTN_LEFT ? 0 : code == BTN_RIGHT ? 1 : code == BTN_MIDDLE ? 2 : -1;
    if (bit < 0) return;
    if (pressed) g_buttons |= 1 << bit;
    else         g_buttons &= ~(1 << bit);
    post_mouse(0, 0);
}

static void wheel_event(int steps)
{
    if (!steps || !in_game()) return;
    int bit = steps > 0 ? 1 << 3 : 1 << 4;
    post_mouse(0, bit);
    post_mouse(0, 0);
}

static void motion_event(int dx)
{
    if (!dx || !in_game()) return;
    post_mouse(dx * 4, 0);
}

static void release_all(void)
{
    for (unsigned c = 0; c < 256; c++) if (g_sent[c]) key_event(c, 0);
    if (g_buttons) { g_buttons = 0; post_mouse(0, 0); }
}

static void drain_tty(int use_keys)
{
    struct pollfd p = { .fd = 0, .events = POLLIN, .revents = 0 };
    if (!g_have_tio || poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;
    char buf[64];
    long n = read(0, buf, sizeof buf);
    if (n <= 0 || !use_keys) return;
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        unsigned char k = 0;
        if (c == 0x1b && i + 2 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
            switch (buf[i + 2]) {
                case 'A': k = KEY_UPARROW;    break;
                case 'B': k = KEY_DOWNARROW;  break;
                case 'C': k = KEY_RIGHTARROW; break;
                case 'D': k = KEY_LEFTARROW;  break;
            }
            i += 2;
        } else if (c == 0x1b) k = KEY_ESCAPE;
        else if (c == '\r' || c == '\n') k = KEY_ENTER;
        else if (c == 0x7f || c == 0x08) k = KEY_BACKSPACE;
        else if (c == '\t') k = KEY_TAB;
        else if (c == ' ') k = KEY_USE;
        else if (c >= 'A' && c <= 'Z') k = (unsigned char)(c - 'A' + 'a');
        else if (c > 0x20 && c < 0x7f) k = c;
        if (in_game()) {
            if (k == 'w') k = KEY_UPARROW;
            else if (k == 's') k = KEY_DOWNARROW;
            else if (k == 'a') k = KEY_STRAFE_L;
            else if (k == 'd') k = KEY_STRAFE_R;
            else if (k == 'e') k = KEY_USE;
        }
        if (!k) continue;
        queue_key(1, k);
        queue_key(0, k);
    }
}

static void drain_evdev(int fd)
{
    evdev_event_t evs[32];
    for (;;) {
        long n = read(fd, evs, sizeof evs);
        if (n <= 0) return;
        size_t count = (size_t)n / sizeof evs[0];
        int dx = 0;
        for (size_t i = 0; i < count; i++) {
            if (evs[i].type == EV_KEY && evs[i].value != 2) {
                if (evs[i].code >= BTN_LEFT && evs[i].code <= BTN_MIDDLE)
                    button_event(evs[i].code, evs[i].value);
                else
                    key_event(evs[i].code, evs[i].value);
            } else if (evs[i].type == EV_REL) {
                if (evs[i].code == REL_X) dx += evs[i].value;
                else if (evs[i].code == REL_WHEEL) wheel_event(evs[i].value);
            }
        }
        motion_event(dx);
        if (count < 32) return;
    }
}

static void drain_window(void)
{
    cervus_fb_event_t ev;
    int dx = 0;
    while (cervus_fb_poll_event(&ev) > 0) {
        switch (ev.type) {
            case CERVUS_FBEV_KEY:     key_event(ev.code, ev.value); break;
            case CERVUS_FBEV_BUTTON:  button_event(ev.code, ev.value); break;
            case CERVUS_FBEV_RELMOVE: dx += ev.x; break;
            case CERVUS_FBEV_WHEEL:   wheel_event(ev.value); break;
            case CERVUS_FBEV_FOCUS:
                g_focused = ev.value;
                if (!g_focused) release_all();
                break;
            case CERVUS_FBEV_CLOSE:   I_Quit(); break;
            default: break;
        }
    }
    if (g_grabbed) motion_event(dx);
    int want = g_focused && in_game();
    if (want != g_grabbed && cervus_fb_grab_pointer(want) == 0) g_grabbed = want;
}

static void drain_input(void)
{
    if (g_windowed) {
        drain_window();
    } else {
        if (g_kbd >= 0) drain_evdev(g_kbd);
        if (g_mouse >= 0) drain_evdev(g_mouse);
        drain_tty(g_kbd < 0);
    }
    if (!g_hint_shown && gamestate == GS_LEVEL) {
        g_hint_shown = 1;
        players[consoleplayer].message = "WASD MOVE, MOUSE TURNS, CLICK FIRES, E USES, SHIFT RUNS";
    }
}

void DG_Init(void)
{
    g_windowed = cervus_fb_windowed();
    if (g_windowed) {
        cervus_fb_info_t disp;
        unsigned s = 1;
        if (cervus_display_info(&disp) == 0 && disp.width >= 1500 && disp.height >= 950) s = 2;
        cervus_fb_set_size(DOOMGENERIC_RESX * s, DOOMGENERIC_RESY * s);
        cervus_fb_set_title("Doom");
    }
    if (cervus_fb_info(&g_fb) != 0) {
        fputs("doom: no framebuffer\n", stderr);
        exit(1);
    }

    g_scale = g_fb.width / DOOMGENERIC_RESX;
    unsigned vscale = g_fb.height / DOOMGENERIC_RESY;
    if (vscale < g_scale) g_scale = vscale;
    if (g_scale < 1) g_scale = 1;

    g_ox = (g_fb.width  - DOOMGENERIC_RESX * g_scale) / 2;
    g_oy = (g_fb.height - DOOMGENERIC_RESY * g_scale) / 2;

    g_line = malloc((size_t)DOOMGENERIC_RESX * g_scale * 4);
    if (!g_line) { fputs("doom: out of memory\n", stderr); exit(1); }

    fputs(HELP, stdout);
    fflush(stdout);

    if (cervus_fb_acquire() != 0) {
        fputs("doom: the screen belongs to a graphical session; start Doom from its\n"
              "      terminal, or from a text console of its own (Ctrl+Alt+F3)\n", stderr);
        exit(1);
    }
    g_acquired = 1;

    mousebfire = 0;
    mousebuse = 1;
    mousebstrafe = -1;
    mousebforward = -1;
    mousebprevweapon = 4;
    mousebnextweapon = 3;

    if (!g_windowed) {
        uint32_t *black = calloc(g_fb.width, 4);
        if (black) {
            for (unsigned y = 0; y < g_fb.height; y++) cervus_fb_blit(black, 0, y, g_fb.width, 1);
            free(black);
        }
        g_kbd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
        g_mouse = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
        evdev_event_t drop[32];
        if (g_kbd >= 0) while (read(g_kbd, drop, sizeof drop) > 0) { }
        if (g_mouse >= 0) while (read(g_mouse, drop, sizeof drop) > 0) { }

        if (tcgetattr(0, &g_tio) == 0) {
            struct termios raw = g_tio;
            raw.c_lflag &= ~(ECHO | ICANON | ISIG);
            raw.c_cc[VMIN]  = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(0, TCSAFLUSH, &raw);
            g_have_tio = 1;
        }
    }
}

void DG_DrawFrame(void)
{
    const uint32_t *src = DG_ScreenBuffer;

    for (unsigned y = 0; y < DOOMGENERIC_RESY; y++) {
        const uint32_t *row = src + (size_t)y * DOOMGENERIC_RESX;
        for (unsigned x = 0; x < DOOMGENERIC_RESX; x++) {
            uint32_t p = row[x];
            for (unsigned s = 0; s < g_scale; s++)
                g_line[x * g_scale + s] = p;
        }
        for (unsigned s = 0; s < g_scale; s++)
            cervus_fb_blit(g_line, g_ox, g_oy + y * g_scale + s,
                           DOOMGENERIC_RESX * g_scale, 1);
    }
    cervus_fb_present();

    drain_input();
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(cervus_uptime_ns() / 1000000ULL);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (g_qtail == g_qhead) drain_input();
    if (g_qtail == g_qhead) return 0;

    unsigned short v = g_queue[g_qtail];
    g_qtail = (g_qtail + 1) % KQ_SIZE;
    *pressed = (v & 0x100) ? 1 : 0;
    *key = (unsigned char)(v & 0xFF);
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

static void restore(void)
{
    if (g_have_tio) tcsetattr(0, TCSAFLUSH, &g_tio);
    if (g_acquired) cervus_fb_release();
    if (g_kbd >= 0) close(g_kbd);
    if (g_mouse >= 0) close(g_mouse);
}

#define WADDIR "/usr/share/doom"

static char **with_default_iwad(int *argc, char **argv)
{
    for (int i = 1; i < *argc; i++)
        if (!strcmp(argv[i], "-iwad")) return argv;

    static const char *wads[] = {
        WADDIR "/freedoom1.wad", WADDIR "/freedoom2.wad",
        WADDIR "/doom.wad",      WADDIR "/doom1.wad",
        WADDIR "/doom2.wad",     NULL
    };

    const char *found = NULL;
    for (int i = 0; wads[i]; i++)
        if (access(wads[i], R_OK) == 0) { found = wads[i]; break; }

    if (!found) {
        fputs("doom: no IWAD in " WADDIR " -- install the freedoom package,\n"
              "      or point at one yourself with -iwad FILE\n", stderr);
        exit(1);
    }

    char **out = malloc((size_t)(*argc + 3) * sizeof *out);
    if (!out) exit(1);
    for (int i = 0; i < *argc; i++) out[i] = argv[i];
    out[*argc]     = "-iwad";
    out[*argc + 1] = (char *)found;
    out[*argc + 2] = NULL;
    *argc += 2;
    return out;
}

int main(int argc, char **argv)
{
    argv = with_default_iwad(&argc, argv);
    atexit(restore);
    doomgeneric_Create(argc, argv);
    for (;;) doomgeneric_Tick();
    return 0;
}
