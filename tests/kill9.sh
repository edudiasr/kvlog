#!/usr/bin/env bash
# Grava chaves com fsync, mata o processo com kill -9 num momento aleatório e confere se
# tudo que foi confirmado continua lá. Repete algumas vezes.
#
#   tests/kill9.sh build/kvlog 10
set -euo pipefail

bin=${1:-build/kvlog}
rounds=${2:-5}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for round in $(seq 1 "$rounds"); do
  rm -rf "$work/db"
  "$bin" "$work/db" fill 100000000 > "$work/acks" &
  pid=$!
  sleep "0.$((RANDOM % 9 + 1))"
  kill -9 "$pid"
  wait "$pid" 2>/dev/null || true
  acked=$(wc -l < "$work/acks")
  printf 'rodada %d: morto depois de %d confirmações -> ' "$round" "$acked"
  "$bin" "$work/db" verify "$acked"
done
