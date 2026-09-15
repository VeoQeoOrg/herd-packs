#!/bin/sh
set -e
SCANNER=$1
XML=../protocol/wayland.xml
$SCANNER public-code       $XML wayland-protocol.c
$SCANNER client-header     $XML wayland-client-protocol.h
$SCANNER -c client-header  $XML wayland-client-protocol-core.h
$SCANNER server-header     $XML wayland-server-protocol.h
$SCANNER -c server-header  $XML wayland-server-protocol-core.h
sed -n 's/^#define WAYLAND_VERSION_MAJOR.*//p' /dev/null
cat > wayland-version.h <<'HEOF'
#ifndef WAYLAND_VERSION_H
#define WAYLAND_VERSION_H
#define WAYLAND_VERSION_MAJOR 1
#define WAYLAND_VERSION_MINOR 24
#define WAYLAND_VERSION_MICRO 0
#define WAYLAND_VERSION "1.24.0"
#define WAYLAND_VERSION_AT_LEAST(major, minor, micro) \
    ((WAYLAND_VERSION_MAJOR > (major)) || \
     (WAYLAND_VERSION_MAJOR == (major) && WAYLAND_VERSION_MINOR > (minor)) || \
     (WAYLAND_VERSION_MAJOR == (major) && WAYLAND_VERSION_MINOR == (minor) && \
      WAYLAND_VERSION_MICRO >= (micro)))
#endif
HEOF
