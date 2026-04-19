#!/usr/bin/env bash
# bench/throughput.sh — sustained-throughput + latency benchmark for zyterm.
#
# Creates a pty pair, runs zyterm in --dump mode against the master side,
# pushes N MiB through the slave, and measures wall time + MiB/s.
#
# Requires: socat (for a scriptable pty pair), GNU time (coreutils).
#
#   bash bench/throughput.sh          # default 16 MiB
#   MB=64 bash bench/throughput.sh    # 64 MiB
set -euo pipefail

MB=${MB:-16}
BIN=${BIN:-./zyterm}

if ! command -v socat >/dev/null 2>&1; then
    echo "skip: socat not installed" >&2
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; kill %1 2>/dev/null || true' EXIT

# Create a bi-directional PTY pair. socat prints both names to stderr.
socat -d -d pty,raw,echo=0,link="$WORK/a" pty,raw,echo=0,link="$WORK/b" &
sleep 0.3

# Producer: push MB MiB of deterministic data into one end.
( dd if=/dev/urandom of="$WORK/a" bs=1M count="$MB" status=none ) &
PROD=$!

# Consumer: zyterm --dump reads the other end for a bounded window.
echo "--- bench: $MB MiB through zyterm --dump ---"
START=$(date +%s.%N)
timeout 30s "$BIN" --dump 10 "$WORK/b" > "$WORK/out" 2>&1 || true
END=$(date +%s.%N)

wait "$PROD" 2>/dev/null || true

BYTES=$(stat -c %s "$WORK/out" 2>/dev/null || echo 0)
ELAPSED=$(awk "BEGIN{printf \"%.3f\", $END - $START}")
MIBPS=$(awk "BEGIN{printf \"%.2f\", ($BYTES/1048576)/$ELAPSED}")

echo "bytes   : $BYTES"
echo "elapsed : ${ELAPSED}s"
echo "rate    : ${MIBPS} MiB/s"
