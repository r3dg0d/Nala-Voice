#!/usr/bin/env bash
# Real-device check of microphone recovery.
#
# Creates a temporary, silent PipeWire *virtual source* ("NalaTestMic"), points a headless
# Nala at it, removes the source (an unplug), brings it back, and prints Nala's events.
# Only that one node is created and removed; no default device is changed and your real
# microphones are not read. Needs PipeWire with pw-loopback and pw-dump, a built ./build/nala,
# and (on NixOS) `nix develop`.
#
# Expected: microphone-open, then microphone-lost ~3 s after the removal, then
# microphone-recovered right after it returns.
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
t="$(mktemp -d)"; pr="$(mktemp -d /tmp/nl-rt.XXXXXX)"   # short path: Qt's control socket needs it
trap 'kill "$(cat "$t/pwl.pid" 2>/dev/null)" "$np" 2>/dev/null; pkill -f "build/nala --config $t" 2>/dev/null; rm -rf "$t" "$pr"' EXIT
mkdir -p "$t/cfg" "$t/state" "$t/data" "$t/cache"; chmod 700 "$pr"
ln -s "$rt/pipewire-0" "$pr/pipewire-0"; ln -s "$rt/pulse" "$pr/pulse"
echo '{"stt.enabled": true, "stt.activation": "always", "stt.device": "NalaTestMic"}' > "$t/cfg/assistant.json"
echo '{}' > "$t/cfg/preferences.json"
log="$t/state/nala/assistant.log"

start_src() { XDG_RUNTIME_DIR="$rt" pw-loopback -n nalatest \
  --capture-props="node.name=nalatest_capture node.passive=true audio.position=[MONO]" \
  --playback-props="media.class=Audio/Source node.name=nala_test_mic node.description=NalaTestMic audio.position=[MONO]" \
  >/dev/null 2>&1 & echo $! > "$t/pwl.pid"; }
have_src() { XDG_RUNTIME_DIR="$rt" pw-dump 2>/dev/null | grep -q '"node.name": "nala_test_mic"'; }
say() { printf '[%s] %s\n' "$(date +%T)" "$1"; }

start_src; sleep 2; have_src || { say "could not create the virtual source"; exit 1; }
cmd=(./build/nala --config "$t/cfg/preferences.json")
command -v nix >/dev/null && [ -f "$here/flake.nix" ] && cmd=(nix develop --command "${cmd[@]}")
(cd "$here" && PULSE_SERVER="unix:$rt/pulse/native" XDG_RUNTIME_DIR="$pr" XDG_CONFIG_HOME="$t/cfg" \
  XDG_STATE_HOME="$t/state" XDG_DATA_HOME="$t/data" XDG_CACHE_HOME="$t/cache" QT_QPA_PLATFORM=offscreen \
  "${cmd[@]}" > "$t/nala.out" 2>&1) & np=$!
for _ in $(seq 1 60); do sleep 1; grep -q microphone-open "$log" 2>/dev/null && break; done
say "microphone opened"; sleep 3
kill "$(cat "$t/pwl.pid")"; sleep 0.5; say "virtual source removed (unplug)"; sleep 8
start_src; say "virtual source restored"; sleep 8
say "events:"; grep -E 'microphone-(open|lost|recovered)' "$log"
