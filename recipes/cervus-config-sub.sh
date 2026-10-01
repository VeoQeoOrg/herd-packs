#!/bin/sh
set -e
find "${1:-.}" -name config.sub -type f | while read -r f; do
    [ -f "$f.real" ] && continue
    mv "$f" "$f.real"
    cat > "$f" <<'WRAP'
#!/bin/sh
case "$1" in *cervus*) echo x86_64-pc-cervus; exit 0 ;; esac
exec "$0.real" "$@"
WRAP
    chmod +x "$f"
done
