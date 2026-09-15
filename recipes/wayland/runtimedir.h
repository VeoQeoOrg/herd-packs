#ifndef CERVUS_WL_RUNTIMEDIR_H
#define CERVUS_WL_RUNTIMEDIR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static inline void ensure_runtime_dir(void)
{
    const char *cur = getenv("XDG_RUNTIME_DIR");
    if (cur && cur[0]) {
        struct stat st;
        if (stat(cur, &st) == 0) return;
        mkdir(cur, 0700);
        return;
    }

    static char dir[64];
    snprintf(dir, sizeof dir, "/run/user/%u", (unsigned)getuid());

    mkdir("/run", 0755);
    mkdir("/run/user", 0755);
    mkdir(dir, 0700);

    setenv("XDG_RUNTIME_DIR", dir, 1);
}

#endif
