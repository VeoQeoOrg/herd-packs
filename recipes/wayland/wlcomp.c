#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/cervus.h>
#include <sys/wait.h>

#include <wayland-server.h>
#include "runtimedir.h"

#define MAX_SURFACES 16

typedef struct {
    struct wl_resource *resource;
    struct wl_resource *buffer;
    struct wl_resource *pending_buffer;
    int                 x, y;
    int                 mapped;
} surface_t;

static struct wl_display  *g_display;
static cervus_fb_info_t    g_fb;
static uint32_t           *g_line;
static surface_t           g_surfaces[MAX_SURFACES];
static int                 g_nsurfaces;
static struct termios      g_tio;
static int                 g_have_tio;
static int                 g_acquired;
static volatile int        g_running = 1;
static int                 g_child;

static surface_t *surface_of(struct wl_resource *r)
{
    for (int i = 0; i < g_nsurfaces; i++)
        if (g_surfaces[i].resource == r) return &g_surfaces[i];
    return NULL;
}

static void paint(void)
{
    int any = 0;
    for (int i = 0; i < g_nsurfaces; i++)
        if (g_surfaces[i].mapped && g_surfaces[i].buffer) any = 1;
    if (!any) return;

    if (!g_acquired) { cervus_fb_acquire(); g_acquired = 1; }

    for (int i = 0; i < g_nsurfaces; i++) {
        surface_t *s = &g_surfaces[i];
        if (!s->mapped || !s->buffer) continue;

        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
        if (!shm) continue;

        wl_shm_buffer_begin_access(shm);
        int      w    = wl_shm_buffer_get_width(shm);
        int      h    = wl_shm_buffer_get_height(shm);
        int      strd = wl_shm_buffer_get_stride(shm);
        uint8_t *data = wl_shm_buffer_get_data(shm);

        if (data && w > 0 && h > 0) {
            if (s->x + w > (int)g_fb.width)  w = (int)g_fb.width  - s->x;
            if (s->y + h > (int)g_fb.height) h = (int)g_fb.height - s->y;
            for (int y = 0; y < h; y++)
                cervus_fb_blit(data + (size_t)y * strd, s->x, s->y + y, w, 1);
        }
        wl_shm_buffer_end_access(shm);

        wl_buffer_send_release(s->buffer);
        s->buffer = NULL;
    }
}

static void surface_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    (void)c; (void)x; (void)y;
    surface_t *s = surface_of(r);
    if (s) s->pending_buffer = buffer;
}

static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb)
{
    (void)r;
    struct wl_resource *callback = wl_resource_create(c, &wl_callback_interface, 1, cb);
    if (!callback) return;
    wl_callback_send_done(callback, (uint32_t)(cervus_uptime_ns() / 1000000ULL));
    wl_resource_destroy(callback);
}

static void surface_noop_region(struct wl_client *c, struct wl_resource *r,
                                struct wl_resource *region)
{
    (void)c; (void)r; (void)region;
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    surface_t *s = surface_of(r);
    if (!s) return;
    if (s->pending_buffer) {
        s->buffer = s->pending_buffer;
        s->pending_buffer = NULL;
        s->mapped = 1;
    }
}

static void surface_transform(struct wl_client *c, struct wl_resource *r, int32_t t)
{
    (void)c; (void)r; (void)t;
}

static void surface_scale(struct wl_client *c, struct wl_resource *r, int32_t s)
{
    (void)c; (void)r; (void)s;
}

static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r,
                                  int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void surface_offset(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y)
{
    (void)c; (void)r; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
    surface_destroy,
    surface_attach,
    surface_damage,
    surface_frame,
    surface_noop_region,
    surface_noop_region,
    surface_commit,
    surface_transform,
    surface_scale,
    surface_damage_buffer,
    surface_offset,
};

static void surface_resource_destroy(struct wl_resource *r)
{
    surface_t *s = surface_of(r);
    if (s) s->mapped = 0;
}

static void compositor_create_surface(struct wl_client *c, struct wl_resource *r,
                                      uint32_t id)
{
    (void)r;
    if (g_nsurfaces >= MAX_SURFACES) return;

    struct wl_resource *res =
        wl_resource_create(c, &wl_surface_interface, 4, id);
    if (!res) { wl_client_post_no_memory(c); return; }

    surface_t *s = &g_surfaces[g_nsurfaces];
    memset(s, 0, sizeof *s);
    s->resource = res;
    s->x = 40 + g_nsurfaces * 30;
    s->y = 40 + g_nsurfaces * 30;
    g_nsurfaces++;

    wl_resource_set_implementation(res, &surface_impl, NULL, surface_resource_destroy);
}

static void region_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c; wl_resource_destroy(r);
}
static void region_add(struct wl_client *c, struct wl_resource *r,
                       int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }

static const struct wl_region_interface region_impl = {
    region_destroy, region_add, region_add,
};

static void compositor_create_region(struct wl_client *c, struct wl_resource *r,
                                     uint32_t id)
{
    (void)r;
    struct wl_resource *res = wl_resource_create(c, &wl_region_interface, 1, id);
    if (!res) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(res, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    compositor_create_surface,
    compositor_create_region,
};

static void bind_compositor(struct wl_client *c, void *data, uint32_t ver, uint32_t id)
{
    (void)data;
    fflush(stderr);
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface,
                                               ver < 4 ? ver : 4, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

static void on_sigint(int sig) { (void)sig; g_running = 0; }

static void restore(void)
{
    if (g_have_tio) tcsetattr(0, TCSAFLUSH, &g_tio);
    if (g_acquired) cervus_fb_release();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    ensure_runtime_dir();

    if (cervus_fb_info(&g_fb) != 0) {
        fputs("wlcomp: no framebuffer\n", stderr);
        return 1;
    }
    g_line = malloc((size_t)g_fb.width * 4);
    if (!g_line) return 1;

    g_display = wl_display_create();
    if (!g_display) { fputs("wlcomp: cannot create display\n", stderr); return 1; }

    const char *sock = wl_display_add_socket_auto(g_display);
    if (!sock) { fputs("wlcomp: cannot listen on a socket\n", stderr); return 1; }

    if (!wl_global_create(g_display, &wl_compositor_interface, 4, NULL, bind_compositor)) {
        fputs("wlcomp: cannot advertise wl_compositor\n", stderr);
        return 1;
    }
    if (wl_display_init_shm(g_display) != 0) {
        fputs("wlcomp: cannot advertise wl_shm\n", stderr);
        return 1;
    }

    printf("wlcomp: listening on %s (%ux%u)\n", sock, g_fb.width, g_fb.height);
    printf("wlcomp: set WAYLAND_DISPLAY=%s for clients; Ctrl-C to stop\n", sock);
    fflush(stdout);

    signal(SIGINT, on_sigint);
    atexit(restore);

    if (argc > 1) {
        setenv("WAYLAND_DISPLAY", sock, 1);
        int pid = fork();
        if (pid == 0) {
            execvp(argv[1], argv + 1);
            fprintf(stderr, "wlcomp: cannot run %s\n", argv[1]);
            _exit(127);
        }
        if (pid > 0) g_child = pid;
    }

    if (tcgetattr(0, &g_tio) == 0) g_have_tio = 1;

    struct wl_event_loop *loop = wl_display_get_event_loop(g_display);
    while (g_running) {
        wl_display_flush_clients(g_display);
        wl_event_loop_dispatch(loop, 16);
        paint();

        if (g_child) {
            int st;
            if (waitpid(g_child, &st, WNOHANG) == g_child) break;
        }
    }

    if (g_child) kill(g_child, SIGTERM);

    wl_display_destroy(g_display);
    return 0;
}
