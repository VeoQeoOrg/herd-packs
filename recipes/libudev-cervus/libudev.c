#include "libudev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

#define PCI_SYSPATH   "/sys/devices/pci0000:00/0000:00:02.0"
#define PCI_SYSNAME   "0000:00:02.0"
#define INPUT_SYSPATH "/sys/devices/platform/cervus-input/input"

#define DRM_MAJOR   226
#define INPUT_MAJOR 13
#define EVDEV_MINOR_BASE 64

#define EVIOC_READ(nr, size) ((2u << 30) | ((unsigned)(size) << 16) | ('E' << 8) | (nr))
#define EVIOCGNAME(len)      EVIOC_READ(0x06, len)
#define EVIOCGBIT(ev, len)   EVIOC_READ(0x20 + (ev), len)
#define EVIOCGID             EVIOC_READ(0x02, 8)

enum kind { K_PCI, K_DRM, K_INPUT, K_EVENT };

struct udev {
    int refs;
    void *userdata;
    int log_priority;
};

struct udev_list_entry {
    struct udev_list_entry *next;
    char *name;
    char *value;
};

struct list {
    struct udev_list_entry *head;
    struct udev_list_entry *tail;
};

struct udev_device {
    int refs;
    struct udev *udev;
    enum kind kind;
    int num;
    struct udev_device *parent;
    int parent_done;
    char syspath[160];
    char devpath[160];
    char sysname[32];
    char sysnum[16];
    char devnode[48];
    char subsystem[16];
    char devtype[16];
    dev_t devnum;
    struct list props;
    struct list attrs;
};

struct udev_enumerate {
    int refs;
    struct udev *udev;
    char subsystems[8][32];
    int nsubsystems;
    char sysnames[8][64];
    int nsysnames;
    struct list result;
};

struct udev_monitor {
    int refs;
    struct udev *udev;
    int fds[2];
};

static struct udev_list_entry *list_add(struct list *l, const char *name, const char *value)
{
    struct udev_list_entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->name = strdup(name);
    e->value = value ? strdup(value) : NULL;
    if (!e->name || (value && !e->value)) {
        free(e->name);
        free(e->value);
        free(e);
        return NULL;
    }
    if (l->tail) l->tail->next = e;
    else l->head = e;
    l->tail = e;
    return e;
}

static void list_clear(struct list *l)
{
    struct udev_list_entry *e = l->head;
    while (e) {
        struct udev_list_entry *n = e->next;
        free(e->name);
        free(e->value);
        free(e);
        e = n;
    }
    l->head = l->tail = NULL;
}

static const char *list_get(const struct list *l, const char *name)
{
    for (struct udev_list_entry *e = l->head; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e->value;
    return NULL;
}

static void prop_num(struct udev_device *d, const char *key, unsigned v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%u", v);
    list_add(&d->props, key, buf);
}

static int bit_set(const unsigned char *bits, unsigned n)
{
    return (bits[n / 8] >> (n % 8)) & 1;
}

struct evinfo {
    char name[128];
    unsigned short id[4];
    int keyboard;
    int key;
    int mouse;
};

static void probe_event(const char *devnode, struct evinfo *info)
{
    memset(info, 0, sizeof *info);
    snprintf(info->name, sizeof info->name, "%s", devnode);
    int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    unsigned char ev[4] = {0}, keys[96] = {0}, rel[2] = {0};
    ioctl(fd, EVIOCGNAME(sizeof info->name - 1), info->name);
    ioctl(fd, EVIOCGID, info->id);
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(1, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(2, sizeof rel), rel);
    close(fd);
    if (bit_set(ev, 1)) {
        info->key = 1;
        if (bit_set(keys, 30) && bit_set(keys, 44)) info->keyboard = 1;
    }
    if (bit_set(ev, 2) && bit_set(rel, 0) && bit_set(rel, 1) && bit_set(keys, 0x110))
        info->mouse = 1;
}

static dev_t node_devnum(const char *devnode)
{
    struct stat st;
    if (stat(devnode, &st) < 0 || !S_ISCHR(st.st_mode)) return 0;
    return (dev_t)st.st_rdev;
}

static void set_paths(struct udev_device *d, const char *syspath)
{
    snprintf(d->syspath, sizeof d->syspath, "%s", syspath);
    snprintf(d->devpath, sizeof d->devpath, "%s", syspath + strlen("/sys"));
}

static struct udev_device *device_build(struct udev *udev, enum kind kind, int num)
{
    struct udev_device *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->refs = 1;
    d->udev = udev_ref(udev);
    d->kind = kind;
    d->num = num;

    char path[160];
    switch (kind) {
    case K_PCI:
        set_paths(d, PCI_SYSPATH);
        snprintf(d->sysname, sizeof d->sysname, "%s", PCI_SYSNAME);
        snprintf(d->subsystem, sizeof d->subsystem, "pci");
        list_add(&d->attrs, "boot_vga", "1");
        list_add(&d->attrs, "vendor", "0x1234");
        list_add(&d->attrs, "device", "0x1111");
        list_add(&d->props, "DRIVER", "cervus-drm");
        list_add(&d->props, "PCI_SLOT_NAME", PCI_SYSNAME);
        break;
    case K_DRM:
        snprintf(path, sizeof path, "%s/drm/card%d", PCI_SYSPATH, num);
        set_paths(d, path);
        snprintf(d->sysname, sizeof d->sysname, "card%d", num);
        snprintf(d->sysnum, sizeof d->sysnum, "%d", num);
        snprintf(d->devnode, sizeof d->devnode, "/dev/dri/card%d", num);
        snprintf(d->subsystem, sizeof d->subsystem, "drm");
        snprintf(d->devtype, sizeof d->devtype, "drm_minor");
        d->devnum = node_devnum(d->devnode);
        if (!d->devnum) goto gone;
        list_add(&d->props, "DEVTYPE", d->devtype);
        list_add(&d->props, "ID_PATH", "pci-" PCI_SYSNAME);
        list_add(&d->props, "ID_FOR_SEAT", "drm-pci-0000_00_02_0");
        break;
    case K_INPUT:
    case K_EVENT: {
        char node[48];
        snprintf(node, sizeof node, "/dev/input/event%d", num);
        dev_t devnum = node_devnum(node);
        if (!devnum) goto gone;
        struct evinfo info;
        probe_event(node, &info);
        snprintf(d->subsystem, sizeof d->subsystem, "input");
        if (kind == K_EVENT) {
            snprintf(path, sizeof path, "%s/input%d/event%d", INPUT_SYSPATH, num, num);
            snprintf(d->sysname, sizeof d->sysname, "event%d", num);
            snprintf(d->devnode, sizeof d->devnode, "%s", node);
            d->devnum = devnum;
        } else {
            snprintf(path, sizeof path, "%s/input%d", INPUT_SYSPATH, num);
            snprintf(d->sysname, sizeof d->sysname, "input%d", num);
            char quoted[140];
            snprintf(quoted, sizeof quoted, "\"%s\"", info.name);
            list_add(&d->props, "NAME", quoted);
            char product[40];
            snprintf(product, sizeof product, "%x/%x/%x/%x",
                     info.id[0], info.id[1], info.id[2], info.id[3]);
            list_add(&d->props, "PRODUCT", product);
            list_add(&d->attrs, "name", info.name);
        }
        set_paths(d, path);
        snprintf(d->sysnum, sizeof d->sysnum, "%d", num);
        list_add(&d->props, "ID_INPUT", "1");
        if (info.key) list_add(&d->props, "ID_INPUT_KEY", "1");
        if (info.keyboard) list_add(&d->props, "ID_INPUT_KEYBOARD", "1");
        if (info.mouse) list_add(&d->props, "ID_INPUT_MOUSE", "1");
        list_add(&d->props, "ID_PATH", "platform-cervus-input");
        break;
    }
    }

    list_add(&d->props, "DEVPATH", d->devpath);
    list_add(&d->props, "SUBSYSTEM", d->subsystem);
    if (d->devnode[0]) {
        list_add(&d->props, "DEVNAME", d->devnode);
        prop_num(d, "MAJOR", major(d->devnum));
        prop_num(d, "MINOR", minor(d->devnum));
    }
    return d;

gone:
    udev_unref(d->udev);
    free(d);
    errno = ENODEV;
    return NULL;
}

static int parse_trailing_int(const char *s, const char *prefix, int *out)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0 || !s[n]) return 0;
    char *end;
    long v = strtol(s + n, &end, 10);
    if (*end || v < 0) return 0;
    *out = (int)v;
    return 1;
}

static struct udev_device *device_from_syspath(struct udev *udev, const char *syspath)
{
    int num;
    if (strcmp(syspath, PCI_SYSPATH) == 0) return device_build(udev, K_PCI, 0);
    if (strncmp(syspath, PCI_SYSPATH "/drm/", strlen(PCI_SYSPATH "/drm/")) == 0 &&
        parse_trailing_int(syspath + strlen(PCI_SYSPATH "/drm/"), "card", &num))
        return device_build(udev, K_DRM, num);
    if (strncmp(syspath, INPUT_SYSPATH "/", strlen(INPUT_SYSPATH "/")) == 0) {
        const char *rest = syspath + strlen(INPUT_SYSPATH "/");
        const char *slash = strchr(rest, '/');
        if (!slash) {
            if (parse_trailing_int(rest, "input", &num)) return device_build(udev, K_INPUT, num);
        } else if (parse_trailing_int(slash + 1, "event", &num)) {
            return device_build(udev, K_EVENT, num);
        }
    }
    errno = ENODEV;
    return NULL;
}

EXPORT struct udev *udev_new(void)
{
    struct udev *u = calloc(1, sizeof *u);
    if (u) {
        u->refs = 1;
        u->log_priority = 3;
    }
    return u;
}

EXPORT struct udev *udev_ref(struct udev *udev)
{
    if (udev) udev->refs++;
    return udev;
}

EXPORT struct udev *udev_unref(struct udev *udev)
{
    if (udev && --udev->refs == 0) free(udev);
    return NULL;
}

EXPORT void *udev_get_userdata(struct udev *udev)
{
    return udev ? udev->userdata : NULL;
}

EXPORT void udev_set_userdata(struct udev *udev, void *userdata)
{
    if (udev) udev->userdata = userdata;
}

EXPORT void udev_set_log_fn(struct udev *udev,
                            void (*log_fn)(struct udev *, int, const char *, int,
                                           const char *, const char *, va_list))
{
    (void)udev;
    (void)log_fn;
}

EXPORT int udev_get_log_priority(struct udev *udev)
{
    return udev ? udev->log_priority : 0;
}

EXPORT void udev_set_log_priority(struct udev *udev, int priority)
{
    if (udev) udev->log_priority = priority;
}

EXPORT struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *e)
{
    return e ? e->next : NULL;
}

EXPORT struct udev_list_entry *udev_list_entry_get_by_name(struct udev_list_entry *e,
                                                           const char *name)
{
    for (; e; e = e->next)
        if (name && strcmp(e->name, name) == 0) return e;
    return NULL;
}

EXPORT const char *udev_list_entry_get_name(struct udev_list_entry *e)
{
    return e ? e->name : NULL;
}

EXPORT const char *udev_list_entry_get_value(struct udev_list_entry *e)
{
    return e ? e->value : NULL;
}

EXPORT struct udev_device *udev_device_ref(struct udev_device *d)
{
    if (d) d->refs++;
    return d;
}

EXPORT struct udev_device *udev_device_unref(struct udev_device *d)
{
    if (!d || --d->refs > 0) return NULL;
    if (d->parent) udev_device_unref(d->parent);
    list_clear(&d->props);
    list_clear(&d->attrs);
    udev_unref(d->udev);
    free(d);
    return NULL;
}

EXPORT struct udev *udev_device_get_udev(struct udev_device *d)
{
    return d ? d->udev : NULL;
}

EXPORT struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath)
{
    if (!udev || !syspath) {
        errno = EINVAL;
        return NULL;
    }
    return device_from_syspath(udev, syspath);
}

static struct udev_device *scan_dir_for_devnum(struct udev *udev, const char *dir,
                                               const char *prefix, enum kind kind, dev_t devnum)
{
    DIR *dp = opendir(dir);
    if (!dp) return NULL;
    struct dirent *de;
    struct udev_device *found = NULL;
    while (!found && (de = readdir(dp))) {
        int num;
        if (!parse_trailing_int(de->d_name, prefix, &num)) continue;
        char node[300];
        snprintf(node, sizeof node, "%s/%s", dir, de->d_name);
        if (node_devnum(node) == devnum) found = device_build(udev, kind, num);
    }
    closedir(dp);
    return found;
}

EXPORT struct udev_device *udev_device_new_from_devnum(struct udev *udev, char type, dev_t devnum)
{
    if (!udev || type != 'c' || !devnum) {
        errno = ENODEV;
        return NULL;
    }
    struct udev_device *d = scan_dir_for_devnum(udev, "/dev/dri", "card", K_DRM, devnum);
    if (!d) d = scan_dir_for_devnum(udev, "/dev/input", "event", K_EVENT, devnum);
    if (!d) errno = ENODEV;
    return d;
}

EXPORT struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *udev,
                                                                  const char *subsystem,
                                                                  const char *sysname)
{
    int num;
    if (!udev || !subsystem || !sysname) {
        errno = EINVAL;
        return NULL;
    }
    if (strcmp(subsystem, "drm") == 0 && parse_trailing_int(sysname, "card", &num))
        return device_build(udev, K_DRM, num);
    if (strcmp(subsystem, "input") == 0) {
        if (parse_trailing_int(sysname, "event", &num)) return device_build(udev, K_EVENT, num);
        if (parse_trailing_int(sysname, "input", &num)) return device_build(udev, K_INPUT, num);
    }
    if (strcmp(subsystem, "pci") == 0 && strcmp(sysname, PCI_SYSNAME) == 0)
        return device_build(udev, K_PCI, 0);
    errno = ENODEV;
    return NULL;
}

EXPORT struct udev_device *udev_device_new_from_device_id(struct udev *udev, const char *id)
{
    unsigned ma, mi;
    char c;
    if (id && sscanf(id, "%c%u:%u", &c, &ma, &mi) == 3 && c == 'c')
        return udev_device_new_from_devnum(udev, 'c', makedev(ma, mi));
    errno = ENODEV;
    return NULL;
}

EXPORT struct udev_device *udev_device_new_from_environment(struct udev *udev)
{
    (void)udev;
    errno = ENODEV;
    return NULL;
}

EXPORT struct udev_device *udev_device_get_parent(struct udev_device *d)
{
    if (!d) return NULL;
    if (!d->parent_done) {
        d->parent_done = 1;
        if (d->kind == K_DRM) d->parent = device_build(d->udev, K_PCI, 0);
        else if (d->kind == K_EVENT) d->parent = device_build(d->udev, K_INPUT, d->num);
    }
    if (!d->parent) errno = ENOENT;
    return d->parent;
}

EXPORT struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *d,
                                                                         const char *subsystem,
                                                                         const char *devtype)
{
    for (struct udev_device *p = udev_device_get_parent(d); p; p = udev_device_get_parent(p)) {
        if (subsystem && strcmp(p->subsystem, subsystem) != 0) continue;
        if (devtype && strcmp(p->devtype, devtype) != 0) continue;
        return p;
    }
    errno = ENOENT;
    return NULL;
}

EXPORT const char *udev_device_get_devpath(struct udev_device *d)
{
    return d ? d->devpath : NULL;
}

EXPORT const char *udev_device_get_subsystem(struct udev_device *d)
{
    return d && d->subsystem[0] ? d->subsystem : NULL;
}

EXPORT const char *udev_device_get_devtype(struct udev_device *d)
{
    return d && d->devtype[0] ? d->devtype : NULL;
}

EXPORT const char *udev_device_get_syspath(struct udev_device *d)
{
    return d ? d->syspath : NULL;
}

EXPORT const char *udev_device_get_sysname(struct udev_device *d)
{
    return d ? d->sysname : NULL;
}

EXPORT const char *udev_device_get_sysnum(struct udev_device *d)
{
    return d && d->sysnum[0] ? d->sysnum : NULL;
}

EXPORT const char *udev_device_get_devnode(struct udev_device *d)
{
    return d && d->devnode[0] ? d->devnode : NULL;
}

EXPORT int udev_device_get_is_initialized(struct udev_device *d)
{
    return d ? 1 : 0;
}

EXPORT struct udev_list_entry *udev_device_get_devlinks_list_entry(struct udev_device *d)
{
    (void)d;
    return NULL;
}

EXPORT struct udev_list_entry *udev_device_get_properties_list_entry(struct udev_device *d)
{
    return d ? d->props.head : NULL;
}

EXPORT struct udev_list_entry *udev_device_get_tags_list_entry(struct udev_device *d)
{
    (void)d;
    return NULL;
}

EXPORT struct udev_list_entry *udev_device_get_current_tags_list_entry(struct udev_device *d)
{
    (void)d;
    return NULL;
}

EXPORT struct udev_list_entry *udev_device_get_sysattr_list_entry(struct udev_device *d)
{
    return d ? d->attrs.head : NULL;
}

EXPORT const char *udev_device_get_property_value(struct udev_device *d, const char *key)
{
    if (!d || !key) return NULL;
    return list_get(&d->props, key);
}

EXPORT const char *udev_device_get_driver(struct udev_device *d)
{
    if (d && d->kind == K_PCI) return "cervus-drm";
    return NULL;
}

EXPORT dev_t udev_device_get_devnum(struct udev_device *d)
{
    return d ? d->devnum : 0;
}

EXPORT const char *udev_device_get_action(struct udev_device *d)
{
    (void)d;
    return NULL;
}

EXPORT unsigned long long int udev_device_get_seqnum(struct udev_device *d)
{
    (void)d;
    return 0;
}

EXPORT unsigned long long int udev_device_get_usec_since_initialized(struct udev_device *d)
{
    (void)d;
    return 0;
}

EXPORT const char *udev_device_get_sysattr_value(struct udev_device *d, const char *sysattr)
{
    if (!d || !sysattr) return NULL;
    return list_get(&d->attrs, sysattr);
}

EXPORT int udev_device_set_sysattr_value(struct udev_device *d, const char *sysattr,
                                         const char *value)
{
    (void)d;
    (void)sysattr;
    (void)value;
    return -EACCES;
}

EXPORT int udev_device_has_tag(struct udev_device *d, const char *tag)
{
    (void)d;
    (void)tag;
    return 0;
}

EXPORT int udev_device_has_current_tag(struct udev_device *d, const char *tag)
{
    (void)d;
    (void)tag;
    return 0;
}

EXPORT struct udev_monitor *udev_monitor_ref(struct udev_monitor *m)
{
    if (m) m->refs++;
    return m;
}

EXPORT struct udev_monitor *udev_monitor_unref(struct udev_monitor *m)
{
    if (!m || --m->refs > 0) return NULL;
    close(m->fds[0]);
    close(m->fds[1]);
    udev_unref(m->udev);
    free(m);
    return NULL;
}

EXPORT struct udev *udev_monitor_get_udev(struct udev_monitor *m)
{
    return m ? m->udev : NULL;
}

EXPORT struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name)
{
    if (!udev) {
        errno = EINVAL;
        return NULL;
    }
    (void)name;
    struct udev_monitor *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    if (pipe(m->fds) < 0) {
        free(m);
        return NULL;
    }
    fcntl(m->fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(m->fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(m->fds[0], F_SETFL, O_NONBLOCK);
    m->refs = 1;
    m->udev = udev_ref(udev);
    return m;
}

EXPORT int udev_monitor_enable_receiving(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_set_receive_buffer_size(struct udev_monitor *m, int size)
{
    (void)size;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_get_fd(struct udev_monitor *m)
{
    return m ? m->fds[0] : -EINVAL;
}

EXPORT struct udev_device *udev_monitor_receive_device(struct udev_monitor *m)
{
    (void)m;
    errno = EAGAIN;
    return NULL;
}

EXPORT int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m,
                                                           const char *subsystem,
                                                           const char *devtype)
{
    (void)subsystem;
    (void)devtype;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_add_match_tag(struct udev_monitor *m, const char *tag)
{
    (void)tag;
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_update(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT int udev_monitor_filter_remove(struct udev_monitor *m)
{
    return m ? 0 : -EINVAL;
}

EXPORT struct udev_enumerate *udev_enumerate_ref(struct udev_enumerate *e)
{
    if (e) e->refs++;
    return e;
}

EXPORT struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e)
{
    if (!e || --e->refs > 0) return NULL;
    list_clear(&e->result);
    udev_unref(e->udev);
    free(e);
    return NULL;
}

EXPORT struct udev *udev_enumerate_get_udev(struct udev_enumerate *e)
{
    return e ? e->udev : NULL;
}

EXPORT struct udev_enumerate *udev_enumerate_new(struct udev *udev)
{
    if (!udev) {
        errno = EINVAL;
        return NULL;
    }
    struct udev_enumerate *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->refs = 1;
    e->udev = udev_ref(udev);
    return e;
}

EXPORT int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *subsystem)
{
    if (!e || !subsystem) return -EINVAL;
    if (e->nsubsystems >= 8) return -ENOMEM;
    snprintf(e->subsystems[e->nsubsystems++], sizeof e->subsystems[0], "%s", subsystem);
    return 0;
}

EXPORT int udev_enumerate_add_nomatch_subsystem(struct udev_enumerate *e, const char *subsystem)
{
    (void)subsystem;
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_match_sysattr(struct udev_enumerate *e, const char *sysattr,
                                            const char *value)
{
    (void)sysattr;
    (void)value;
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_nomatch_sysattr(struct udev_enumerate *e, const char *sysattr,
                                              const char *value)
{
    (void)sysattr;
    (void)value;
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_match_property(struct udev_enumerate *e, const char *property,
                                             const char *value)
{
    (void)property;
    (void)value;
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *sysname)
{
    if (!e || !sysname) return -EINVAL;
    if (e->nsysnames >= 8) return -ENOMEM;
    snprintf(e->sysnames[e->nsysnames++], sizeof e->sysnames[0], "%s", sysname);
    return 0;
}

EXPORT int udev_enumerate_add_match_tag(struct udev_enumerate *e, const char *tag)
{
    (void)tag;
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_match_parent(struct udev_enumerate *e, struct udev_device *parent)
{
    (void)parent;
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_match_is_initialized(struct udev_enumerate *e)
{
    return e ? 0 : -EINVAL;
}

EXPORT int udev_enumerate_add_syspath(struct udev_enumerate *e, const char *syspath)
{
    if (!e || !syspath) return -EINVAL;
    return list_add(&e->result, syspath, NULL) ? 0 : -ENOMEM;
}

static int subsystem_wanted(struct udev_enumerate *e, const char *subsystem)
{
    if (!e->nsubsystems) return 1;
    for (int i = 0; i < e->nsubsystems; i++)
        if (fnmatch(e->subsystems[i], subsystem, 0) == 0) return 1;
    return 0;
}

static int sysname_wanted(struct udev_enumerate *e, const char *sysname)
{
    if (!e->nsysnames) return 1;
    for (int i = 0; i < e->nsysnames; i++)
        if (fnmatch(e->sysnames[i], sysname, 0) == 0) return 1;
    return 0;
}

static void scan_class(struct udev_enumerate *e, const char *dir, const char *prefix,
                       const char *subsystem, enum kind kind)
{
    if (!subsystem_wanted(e, subsystem)) return;
    DIR *dp = opendir(dir);
    if (!dp) return;
    int nums[64];
    int count = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && count < 64) {
        int num;
        if (parse_trailing_int(de->d_name, prefix, &num) && sysname_wanted(e, de->d_name))
            nums[count++] = num;
    }
    closedir(dp);
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && nums[j - 1] > nums[j]; j--) {
            int t = nums[j];
            nums[j] = nums[j - 1];
            nums[j - 1] = t;
        }
    for (int i = 0; i < count; i++) {
        struct udev_device *d = device_build(e->udev, kind, nums[i]);
        if (!d) continue;
        list_add(&e->result, d->syspath, NULL);
        udev_device_unref(d);
    }
}

EXPORT int udev_enumerate_scan_devices(struct udev_enumerate *e)
{
    if (!e) return -EINVAL;
    list_clear(&e->result);
    scan_class(e, "/dev/dri", "card", "drm", K_DRM);
    scan_class(e, "/dev/input", "event", "input", K_EVENT);
    return 0;
}

EXPORT int udev_enumerate_scan_subsystems(struct udev_enumerate *e)
{
    if (!e) return -EINVAL;
    list_clear(&e->result);
    list_add(&e->result, "/sys/class/drm", NULL);
    list_add(&e->result, "/sys/class/input", NULL);
    return 0;
}

EXPORT struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
    return e ? e->result.head : NULL;
}
