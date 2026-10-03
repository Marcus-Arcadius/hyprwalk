#!/usr/bin/env bash
# usage: api.sh <outfile> key=value ... ; polite GET on a MediaWiki API (API env var, default Commons)
# a UA without contact info counts as "unidentified" (10 req/min): keep >= 7 s between requests
out=$1; shift
UA='hypr3d-lipsync-vowel-survey/0.1 (one-off research script for a lip sync test set) curl'
args=()
for kv in "$@"; do args+=(--data-urlencode "$kv"); done
stamp=/tmp/claude-1000/-home-monero-Documents-3D/085051d7-540b-4886-ae17-dbd3dfc7c646/scratchpad/speech/tools/.last_${HOSTTAG:-commons}
for try in 1 2 3 4; do
  if [ -f "$stamp" ]; then
    now=$(date +%s); last=$(cat "$stamp"); d=$(( ${GAP:-7} - (now - last) )); [ $d -gt 0 ] && sleep $d
  fi
  date +%s > "$stamp"
  code=$(curl -sS --compressed -A "$UA" -D "$out.hdr" --get "${API:-https://commons.wikimedia.org/w/api.php}" --data-urlencode format=json --data-urlencode formatversion=2 "${args[@]}" -o "$out" -w '%{http_code}')
  if [ "$code" = 200 ]; then rm -f "$out.hdr"; exit 0; fi
  ra=$(grep -i '^retry-after:' "$out.hdr" | tr -dc '0-9'); ra=${ra:-30}
  echo "HTTP $code, retry after $ra s" >&2
  sleep $((ra + 5))
done
exit 1
