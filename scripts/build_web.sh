#!/usr/bin/env bash
# build_web.sh — bash mirror of build_web.ps1 (needs: gzip, sha256sum, python3
# OR node for JSON parsing; falls back to a grep-based parser if neither).
# Writes data/app.bundle.js(.gz), style.css.gz, index.html.gz, assets.json.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$ROOT/data"
SRC="$DATA/web-assets.json"
[ -f "$SRC" ] || { echo "missing $SRC" >&2; exit 1; }

# Extract the "scripts" / "styles" arrays without a JSON tool: pull the block
# between the key and the closing bracket, keep quoted strings.
json_list() {
  sed -n "/\"$1\"[[:space:]]*:/,/\]/p" "$SRC" | grep -o '"[^"]*\.\(js\|css\)"' | tr -d '"'
}
mapfile -t SCRIPTS < <(json_list scripts)
mapfile -t STYLES  < <(json_list styles)
[ "${#SCRIPTS[@]}" -gt 0 ] || { echo "no scripts in web-assets.json" >&2; exit 1; }

for f in "${SCRIPTS[@]}" "${STYLES[@]}" index.html; do
  [ -f "$DATA/$f" ] || { echo "web-assets.json references missing file: $f" >&2; exit 1; }
done

VERSION="$( (for f in "${SCRIPTS[@]}" "${STYLES[@]}" index.html; do cat "$DATA/$f"; done) | sha256sum | cut -c1-12)"

BUNDLE="$DATA/app.bundle.js"
: > "$BUNDLE"
for f in "${SCRIPTS[@]}"; do
  printf '\n;/* ---- %s ---- */\n' "$f" >> "$BUNDLE"
  cat "$DATA/$f" >> "$BUNDLE"
  printf '\n' >> "$BUNDLE"
done
gzip -9 -n -c "$BUNDLE" > "$BUNDLE.gz"          # -n: no name/mtime → deterministic
for c in "${STYLES[@]}"; do gzip -9 -n -c "$DATA/$c" > "$DATA/$c.gz"; done
gzip -9 -n -c "$DATA/index.html" > "$DATA/index.html.gz"

# Every other root web file, gzipped beside it: the K10 caches each one in
# PSRAM and a .gz takes the plain copy's place (see build_web.ps1).
for p in "$DATA"/*.js "$DATA"/*.css "$DATA"/*.html "$DATA"/*.json; do
  [ -f "$p" ] || continue
  n="$(basename "$p")"
  case "$n" in .*|app.bundle.js|index.html|assets.json) continue ;; esac
  for c in "${STYLES[@]}"; do [ "$n" = "$c" ] && continue 2; done
  gzip -9 -n -c "$p" > "$p.gz.tmp"
  if [ "$(stat -c%s "$p.gz.tmp" 2>/dev/null || wc -c < "$p.gz.tmp")" -lt $(( $(stat -c%s "$p" 2>/dev/null || wc -c < "$p") * 9 / 10 )) ]; then
    mv "$p.gz.tmp" "$p.gz"
  else
    rm -f "$p.gz.tmp" "$p.gz"
  fi
done

size() { stat -c%s "$1" 2>/dev/null || wc -c < "$1"; }

{
  printf '{\n  "version": "%s",\n  "generated": "%s",\n  "mode": "bundle",\n' \
    "$VERSION" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '  "styles": ['
  first=1
  for c in "${STYLES[@]}"; do
    [ $first = 1 ] || printf ','
    first=0
    printf '\n    {"url": "%s", "size": %d, "gz": %d}' "$c" "$(size "$DATA/$c")" "$(size "$DATA/$c.gz")"
  done
  printf '\n  ],\n  "scripts": [\n    {"url": "app.bundle.js", "size": %d, "gz": %d, "sources": [' \
    "$(size "$BUNDLE")" "$(size "$BUNDLE.gz")"
  first=1
  for f in "${SCRIPTS[@]}"; do
    [ $first = 1 ] || printf ', '
    first=0
    printf '"%s"' "$f"
  done
  printf ']}\n  ]\n}\n'
} > "$DATA/assets.json"

echo "[build_web] version $VERSION"
echo "[build_web] app.bundle.js $(size "$BUNDLE") B -> $(size "$BUNDLE.gz") B gz (${#SCRIPTS[@]} files)"
