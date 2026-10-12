#!/usr/bin/env bash
# Sync this checkout to the build host, build one firmware project there, and fetch its artifacts.
# usage: scripts/remote-build.sh <project under firmware/> [board: 2_8 (default) | 1_69]
set -euo pipefail
project=${1:?project under firmware/}
board=${2:-2_8}
root=$(cd "$(dirname "$0")/.." && pwd)
host=${WLC_BUILD_HOST:?Set WLC_BUILD_HOST to your owned build machine}
build="build-$project-$board"
# Reuses a control master if one is up; never starts a persistent one, which would hold
# this script's stdout open.
ssh_opts=(-o "ControlPath=$HOME/.ssh/cm-%r@%h:%p")
rsync -a --delete --exclude .venv --exclude .git --exclude evidence --exclude artifacts \
  --exclude 'build*' --exclude managed_components --exclude sdkconfig \
  -e "ssh ${ssh_opts[*]}" "$root/" "$host:wlc-c5-touch/src/"
# reconfigure: SRC_DIRS globs are only evaluated at configure time.
ssh "${ssh_opts[@]}" "$host" "set -e; . ~/wlc-c5-touch/env.sh; cd ~/wlc-c5-touch/src/firmware/$project; \
  idf.py -B ~/wlc-c5-touch/$build -DIDF_TARGET=esp32c5 -DWLC_DEPS_DIR=\$WLC_DEPS_DIR -DWLC_BOARD=$board \
    reconfigure build > ~/wlc-c5-touch/$build.log 2>&1 \
  || { grep -nE 'error:|Error' ~/wlc-c5-touch/$build.log | head -30; exit 1; }; \
  if [ -f check_build.py ]; then python check_build.py --build ~/wlc-c5-touch/$build --config ~/wlc-c5-touch/$build/sdkconfig 2>/dev/null \
    || python check_build.py --build ~/wlc-c5-touch/$build --config sdkconfig; fi"
echo "build ok: $project ($board)"
out="$root/artifacts/$project-$board"; rm -rf "$out"; mkdir -p "$out"
ssh "${ssh_opts[@]}" "$host" "cd ~/wlc-c5-touch/$build && \
  python3 -c 'import json;print(\"\n\".join(json.load(open(\"flasher_args.json\"))[\"flash_files\"].values()))' \
  | tar -cf - -T - flasher_args.json $project.elf sdkconfig" | tar -xf - -C "$out"
# The component lock lives in the repo so every build resolves the same managed components.
scp -q "${ssh_opts[@]}" "$host:wlc-c5-touch/src/firmware/$project/dependencies.lock*" "$root/firmware/$project/" 2>/dev/null || true
if [ "$project" = wlc_sensor ]; then
  scp -q "${ssh_opts[@]}" "$host:wlc-c5-touch/$build/combined-build-report.json" "$out/"
  mkdir -p "$out/esp-idf/main/generated"
  scp -q "${ssh_opts[@]}" "$host:wlc-c5-touch/$build/esp-idf/main/generated/source-manifest.json" "$out/esp-idf/main/generated/"
fi
ls -la "$out"
