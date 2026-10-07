#!/bin/sh
set -e
root="${1:-.}"
find "$root" -name config.sub -type f | while read -r f; do
    [ -f "$f.real" ] && continue
    mv "$f" "$f.real"
    cat > "$f" <<'WRAP'
#!/bin/sh
case "$1" in *cervus*) echo x86_64-pc-cervus; exit 0 ;; esac
exec /bin/sh "$0.real" "$@"
WRAP
    chmod +x "$f"
done
find "$root" -name configure -type f | while read -r f; do
    grep -q 'cervus\* | linux\* |' "$f" && continue
    sed -i -E '/^[[:space:]]*#/! s/(^|[[:space:]])linux\* \|([^)]*\))/\1cervus* | linux* |\2/' "$f"
done
