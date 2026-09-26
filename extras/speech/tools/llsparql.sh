#!/usr/bin/env bash
# usage: llsparql.sh 'QUERY' ; polite GET on the Lingua Libre SPARQL endpoint
sleep 3
curl -sS -m 90 -A 'hypr3d-lipsync-vowel-survey/0.1 (one-off research script) curl' -H 'Accept: application/sparql-results+json' -G 'https://lingualibre.org/bigdata/namespace/wdq/sparql' --data-urlencode "query=$1"
