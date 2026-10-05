#!/usr/bin/env bash
# polite downloader: dl.sh LISTFILE OUTDIR ; saves with the decoded original file name; honours 429 Retry-After
UA='hyprwalk-lipsync-vowel-survey/0.1 (one-off research script for a lip sync test set) curl'
out=$2; mkdir -p "$out"
while read -r url; do
  [ -z "$url" ] && continue
  name=$(python3 -c 'import sys,urllib.parse;print(urllib.parse.unquote(sys.argv[1].rsplit("/",1)[1]))' "$url")
  if [ -s "$out/$name" ]; then echo "have $name"; continue; fi
  for try in 1 2 3 4 5; do
    sleep ${GAP:-8}
    code=$(curl -sS -A "$UA" -D "$out/.hdr" -o "$out/$name" -w '%{http_code}' "$url")
    echo "$code $name (try $try)"
    [ "$code" = 200 ] && break
    rm -f "$out/$name"
    ra=$(grep -i '^retry-after:' "$out/.hdr" | tr -dc '0-9'); ra=${ra:-60}
    sleep $((ra + 10))
  done
done < "$1"
rm -f "$out/.hdr"
