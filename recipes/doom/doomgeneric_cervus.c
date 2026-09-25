#include "doomkeys.h"
#include "m_argv.h"
#include "doomgeneric.h"

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

#define EV_KEY 0x01

typedef struct {
    uint64_t sec;
    uint64_t usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed)) cervus_input_event_t;

#define KQ_SIZE 64

static cervus_fb_info_t  g_fb;
static uint32_t         *g_line;
static unsigned          g_scale;
static unsigned          g_ox, g_oy;
static int               g_kbd = -1;
static struct termios    g_tio;
static int               g_have_tio;
static int               g_acquired;

static unsigned short    g_queue[KQ_SIZE];
static unsigned          g_qhead, g_qtail;
static int               g_evdev_arrows;

static int is_arrow(unsigned char k)
{
    return k == KEY_UPARROW || k == KEY_DOWNARROW ||
           k == KEY_LEFTARROW || k == KEY_RIGHTARROW;
}

static unsigned char scancode_to_doom(uint16_t raw)
{
    uint16_t sc = raw & 0xFF;
    static const unsigned char letters[] = {
        [0x1E] = 'a', [0x30] = 'b', [0x2E] = 'c', [0x20] = 'd',
        [0x12] = 'e', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
        [0x17] = 'i', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
        [0x32] = 'm', [0x31] = 'n', [0x18] = 'o', [0x19] = 'p',
        [0x10] = 'q', [0x13] = 'r', [0x1F] = 's', [0x14] = 't',
        [0x16] = 'u', [0x2F] = 'v', [0x11] = 'w', [0x2D] = 'x',
        [0x15] = 'y', [0x2C] = 'z',
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
        [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
        [0x0A] = '9', [0x0B] = '0',
    };

    switch (sc) {
        case 0x01: return KEY_ESCAPE;
        case 0x1C: return KEY_ENTER;
        case 0x0F: return KEY_TAB;
        case 0x0E: return KEY_BACKSPACE;
        case 0x39: return KEY_USE;
        case 0x1D: return KEY_FIRE;
        case 0x2A: case 0x36: return KEY_RSHIFT;
        case 0x38: return KEY_RALT;
        case 0x48: case 0x47: case 0x49: return KEY_UPARROW;
        case 0x50: case 0x4F: case 0x51: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        default: break;
    }
    if (sc < sizeof letters && letters[sc]) return letters[sc];
    return 0;
}

static void queue_key(int pressed, unsigned char key)
{
    unsigned next = (g_qhead + 1) % KQ_SIZE;
    if (next == g_qtail) return;
    g_queue[g_qhead] = (unsigned short)((pressed ? 0x100 : 0) | key);
    g_qhead = next;
}

static unsigned char tty_key(unsigned char c)
{
    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 0x7f || c == 0x08) return KEY_BACKSPACE;
    if (c == '\t') return KEY_TAB;
    if (c == ' ') return KEY_USE;
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
    if (c > 0x20 && c < 0x7f) return c;
    return 0;
}

static void tty_input(void)
{
    struct pollfd p = { .fd = 0, .events = POLLIN, .revents = 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return;

    char buf[64];
    long n = read(0, buf, sizeof buf);
    if (n <= 0) return;

    int only_arrows = g_kbd >= 0;
    for (long i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        unsigned char k = 0;
        if (c == '\x1b') {
            if (i + 2 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
                switch (buf[i + 2]) {
                    case 'A': k = KEY_UPARROW;    break;
                    case 'B': k = KEY_DOWNARROW;  break;
                    case 'C': k = KEY_RIGHTARROW; break;
                    case 'D': k = KEY_LEFTARROW;  break;
                    default:  break;
                }
                i += 2;
                while (i < n && ((unsigned char)buf[i] < 0x40 || (unsigned char)buf[i] > 0x7e)) i++;
                if (only_arrows && g_evdev_arrows) k = 0;
            } else if (!only_arrows) {
                k = KEY_ESCAPE;
            }
        } else if (!only_arrows) {
            k = tty_key(c);
        }
        if (!k) continue;
        queue_key(1, k);
        queue_key(0, k);
    }
}

static void drop_evdev(void)
{
    close(g_kbd);
    g_kbd = -1;
    g_evdev_arrows = 0;
}

static int kbd_ready(void)
{
    struct pollfd p = { .fd = g_kbd, .events = POLLIN, .revents = 0 };
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) { drop_evdev(); return 0; }
    return (p.revents & POLLIN) != 0;
}

static void drain_input(void)
{
    cervus_input_event_t evs[32];
    while (g_kbd >= 0 && kbd_ready()) {
        long n = read(g_kbd, evs, sizeof evs);
        if (n < 0) { drop_evdev(); break; }
        if (n == 0) break;
        size_t count = (size_t)n / sizeof evs[0];
        for (size_t i = 0; i < count; i++) {
            if (evs[i].type != EV_KEY) continue;
            unsigned char k = scancode_to_doom(evs[i].code);
            if (!k) continue;
            if (is_arrow(k)) g_evdev_arrows = 1;
            queue_key(evs[i].value != 0, k);
        }
        if (count < 32) break;
    }
    tty_input();
}

void DG_Init(void)
{
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

    g_kbd = open("/dev/input0", O_RDONLY);
    if (g_kbd >= 0) {
        cervus_input_event_t drop[32];
        while (kbd_ready() && read(g_kbd, drop, sizeof drop) > 0) { }
    }

    if (tcgetattr(0, &g_tio) == 0) {
        struct termios raw = g_tio;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &raw);
        g_have_tio = 1;
    }

}

void DG_DrawFrame(void)
{
    const uint32_t *src = DG_ScreenBuffer;

    if (!g_acquired) { cervus_fb_acquire(); g_acquired = 1; }

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
