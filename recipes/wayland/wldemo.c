#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mman_shared.h>
#include <math.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "runtimedir.h"

#define W 320
#define H 240

static struct wl_display    *display;
static struct wl_registry   *registry;
static struct wl_compositor *compositor;
static struct wl_shm        *shm;
static struct wl_surface    *surface;
static struct wl_buffer     *buffer;
static uint32_t             *pixels;
static int                   running = 1;
static struct xdg_wm_base   *wm_base;
static struct xdg_surface   *xsurface;
static struct xdg_toplevel  *toplevel;
static int                   configured;

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
    (void)d;
    xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static void xsurface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}

static const struct xdg_surface_listener xsurface_listener = { xsurface_configure };

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *states)
{
    (void)d; (void)t; (void)w; (void)h; (void)states;
}

static void toplevel_close(void *d, struct xdg_toplevel *t)
{
    (void)d; (void)t;
    running = 0;
}

static void toplevel_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{
    (void)d; (void)t; (void)w; (void)h;
}

static void toplevel_caps(void *d, struct xdg_toplevel *t, struct wl_array *caps)
{
    (void)d; (void)t; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps,
};

static void registry_global(void *d, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t ver)
{
    (void)d; (void)ver;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    }
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global, registry_remove,
};

static void buffer_release(void *d, struct wl_buffer *b)
{
    (void)d; (void)b;
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void draw(int frame)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int cx = x - W / 2;
            int cy = y - H / 2;
            int d  = (int)(cx * cx + cy * cy) / 64;

            uint8_t r = (uint8_t)(128 + 127 * sin((d - frame) * 0.12));
            uint8_t g = (uint8_t)(128 + 127 * sin((d - frame) * 0.12 + 2.1));
            uint8_t b = (uint8_t)(128 + 127 * sin((d - frame) * 0.12 + 4.2));

            if (x < 3 || y < 3 || x >= W - 3 || y >= H - 3) { r = g = b = 255; }
            pixels[y * W + x] = 0xFF000000u | ((uint32_t)r << 16) |
                                ((uint32_t)g << 8) | b;
        }
    }
}

int main(void)
{
    ensure_runtime_dir();

    display = wl_display_connect(NULL);
    if (!display) {
        if (!getenv("WAYLAND_DISPLAY") && !getenv("WLDEMO_UNDER_WLCOMP")) {
            fputs("wldemo: no Wayland compositor is running, starting wlcomp for it\n", stderr);
            setenv("WLDEMO_UNDER_WLCOMP", "1", 1);
            execlp("wlcomp", "wlcomp", "wldemo", (char *)NULL);
        }
        fputs("wldemo: no Wayland compositor to connect to -- run it inside Weston\n"
              "        (startweston) or under wlcomp (wlcomp wldemo)\n", stderr);
        return 1;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm) {
        fputs("wldemo: the compositor offers no wl_compositor/wl_shm\n", stderr);
        return 1;
    }

    int stride = W * 4;
    int size   = stride * H;

    int fd = memfd_create("wldemo", 0);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        fputs("wldemo: cannot make a shared buffer\n", stderr);
        return 1;
    }
    pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) { fputs("wldemo: cannot map it\n", stderr); return 1; }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    buffer = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    if (wm_base) {
        xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
        xdg_surface_add_listener(xsurface, &xsurface_listener, NULL);
        toplevel = xdg_surface_get_toplevel(xsurface);
        xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
        xdg_toplevel_set_title(toplevel, "wldemo");
        xdg_toplevel_set_app_id(toplevel, "wldemo");
        wl_surface_commit(surface);
        while (!configured && wl_display_dispatch(display) >= 0) { }
    }

    printf("wldemo: drawing a %dx%d window through Wayland\n", W, H);
    fflush(stdout);

    for (int frame = 0; running && frame < 100000; frame++) {
        draw(frame);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, W, H);
        wl_surface_commit(surface);
        if (wl_display_roundtrip(display) < 0) break;
        usleep(30000);
    }

    wl_display_disconnect(display);
    return 0;
}
