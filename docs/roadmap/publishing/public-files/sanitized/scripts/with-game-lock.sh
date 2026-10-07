#!/bin/bash
# Serialise every use of the ONE game folder: launching the game, and deploying into it.
#
# Why this exists. the development guidelines rule 0d already says one session owns the game folder at a
# time, but nothing enforced it, and on 2026-09-07 three agents worked in this worktree at
# once. The damage was not theoretical:
#
#   * two of them deployed over a third mid-campaign (stamps at 10:12 and 10:20), so half
#     its captures came from a different binary than the other half;
#   * concurrent game instances starved each other -- one measurement run rendered 88-224
#     frames by t=9 s against 1117 on a quiet run, which moved a "must not change" control
#     box from 0.43 to 3.9 and made two arms of the same A/B incomparable;
#   * a deploy could not restamp DEPLOYED-FROM.txt because another instance held
#     wgpu_renderer.dll open, so the stamp lied about what was installed.
#
# None of that announces itself. A contended capture looks exactly like a real regression.
#
# Usage:
#   scripts/with-game-lock.sh <command...>
#   LOCK_OWNER="smoke A/B" scripts/with-game-lock.sh bash run-capture.sh
#
# Wraps ANY command: a capture batch, Deploy.ps1, a build+deploy script. Waits for the
# lock, runs the command, always releases (including on Ctrl+C or a crash, via trap).

set -u

LOCK="${POSEIDON_GAME_LOCK:-/tmp/poseidon-gamefolder.lock}"
# A capture campaign can legitimately hold this for a while; anything past this is a
# crashed holder. See the "stale locks survive a crash" lesson -- a lock directory left by
# a killed process makes every other agent wait for ever, and the symptom is silence.
STALE_SECONDS="${POSEIDON_GAME_LOCK_STALE:-1800}"
WAIT_NOTICE=60

start=$(date +%s)
notified=0
while true; do
    if mkdir "$LOCK" 2>/dev/null; then
        break
    fi
    holder="$(cat "$LOCK/owner" 2>/dev/null || echo unknown)"
    lockAge=$(( $(date +%s) - $(stat -c %Y "$LOCK" 2>/dev/null || date +%s) ))
    if [ "$lockAge" -gt "$STALE_SECONDS" ]; then
        echo "game-lock: breaking a stale lock held ${lockAge}s by: $holder" >&2
        rm -rf "$LOCK" 2>/dev/null
        continue
    fi
    waited=$(( $(date +%s) - start ))
    if [ "$notified" = "0" ] && [ "$waited" -ge "$WAIT_NOTICE" ]; then
        notified=1
        echo "game-lock: waiting ${waited}s for: $holder" >&2
    fi
    sleep 5
done
printf '%s pid=%s %s\n' "${LOCK_OWNER:-unnamed}" "$$" "$(date -Is)" > "$LOCK/owner"
trap 'rm -rf "$LOCK"' EXIT INT TERM

# The lock says nobody ELSE holding it will start a game. It cannot say nobody bypassed it
# -- the owner playing the game themselves, or an agent that has not been told about this
# script. Refuse rather than measure against a contended machine.
running=$(powershell -NoProfile -Command "(Get-Process OpenPoseidon -ErrorAction SilentlyContinue | Measure-Object).Count" 2>/dev/null | tr -d '\r')
if [ -n "$running" ] && [ "$running" != "0" ]; then
    echo "game-lock: REFUSED, $running OpenPoseidon instance(s) already running outside the lock" >&2
    exit 4
fi

"$@"
