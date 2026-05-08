#!/usr/bin/env bash
# sync_data.sh — bash mirror of sync_data.ps1.
# Usage: ./scripts/sync_data.sh <host[:port]> [--dry-run]
#
# POSTs each file under data/ to http://<host>/upload?dest=/data/<rel>.
# No manifest / skip-on-hash here — keep it simple; PowerShell version handles
# incremental sync. This is for one-shot pushes from MSYS bash on Windows.

set -euo pipefail

HOST="${1:-}"
[ -z "$HOST" ] && { echo "Usage: $0 <host[:port]> [--dry-run]" >&2; exit 1; }
DRY=0
[ "${2:-}" = "--dry-run" ] && DRY=1

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$ROOT/data"

PUSHED=0; FAILED=0
while IFS= read -r -d '' f; do
  rel="${f#$DATA/}"
  [ "$rel" = ".upload-manifest.json" ] && continue
  url="http://$HOST/upload?dest=/data/$rel"
  printf '  %7d bytes  %s\n' "$(stat -c%s "$f" 2>/dev/null || wc -c < "$f")" "$rel"
  if [ "$DRY" = 0 ]; then
    if curl --fail --silent --show-error --limit-rate 50K \
         --data-binary "@$f" -X POST "$url" >/dev/null; then
      PUSHED=$((PUSHED+1))
      sleep 0.5
    else
      echo "  FAIL $rel" >&2
      FAILED=$((FAILED+1))
    fi
  fi
done < <(find "$DATA" -type f -print0)

echo
echo "[sync] $PUSHED pushed, $FAILED failed"
[ "$FAILED" -gt 0 ] && exit 1
exit 0
