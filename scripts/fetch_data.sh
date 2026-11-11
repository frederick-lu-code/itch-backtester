#!/usr/bin/env bash
# Downloads one Nasdaq TotalView-ITCH 5.0 sample day into data/ and gunzips it.
#
#   scripts/fetch_data.sh                      # the project's reference day
#   scripts/fetch_data.sh 01302019.NASDAQ_ITCH50.gz
#   KEEP_GZ=1 scripts/fetch_data.sh            # keep the compressed download as well
#
# data/ is git-ignored: Nasdaq data must never be committed. An interrupted download resumes
# where it stopped when the script is run again.
set -euo pipefail

FILE="${1:-12302019.NASDAQ_ITCH50.gz}"
BASE_URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"

if [[ "$FILE" != *.gz ]]; then
    echo "error: expected a .gz file name, got '$FILE'" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$ROOT/data"
GZ="$DATA/$FILE"
OUT="${GZ%.gz}"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$DATA"

if [[ -f "$OUT" ]]; then
    echo "$OUT already exists; nothing to do."
    exit 0
fi

if [[ ! -f "$GZ" ]]; then
    echo "Downloading $BASE_URL/$FILE"
    curl --fail --location --retry 5 --retry-delay 5 --continue-at - \
        --output "$GZ.part" "$BASE_URL/$FILE"
    mv "$GZ.part" "$GZ"
fi

# Nasdaq's .md5sum files are listed next to the data but the server does not serve them, so
# the gzip CRC is the integrity check available.
echo "Verifying gzip integrity"
gzip -t "$GZ"

echo "Decompressing to $OUT"
gunzip -c "$GZ" >"$OUT.tmp"
mv "$OUT.tmp" "$OUT"

if [[ "${KEEP_GZ:-0}" != "1" ]]; then
    rm "$GZ"
fi

echo "File:    $OUT"
echo "Bytes:   $(wc -c <"$OUT" | tr -d ' ')"
echo "SHA-256: $(sha256 "$OUT")"
