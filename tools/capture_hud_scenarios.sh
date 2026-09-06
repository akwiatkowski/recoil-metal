#!/usr/bin/env bash
# Run from the repository root. Assets and generated images never enter git.
set -euo pipefail

capture_dir=${1:-build/hud-acceptance}
fa_install=${FA_INSTALL:-/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance}
capture_binary=${CAPTURE_BINARY:-./build/recoil-metal}
capture_width=${CAPTURE_WIDTH:-1600}
capture_height=${CAPTURE_HEIGHT:-900}
capture_backing=${CAPTURE_BACKING:-1}
test -f "$fa_install/maps/SCMP_009/SCMP_009.scmap"
test -x "$capture_binary"
mkdir -p "$capture_dir"

capture() {
    local name=$1 seconds=$2 fixture=$3 expected=$4
    shift 4
    local pass
    for pass in first repeat; do
        local hashes=(--hash-log "$capture_dir/$name.hashes")
        if [[ $pass == repeat ]]; then
            hashes=(--check-hash-log "$capture_dir/$name.hashes")
        fi
        if ! mise exec -- "$capture_binary" "$fa_install/maps/SCMP_009/SCMP_009.scmap" \
            --gamedata "$fa_install/gamedata" --skirmish --armies 2 \
            --play "$seconds" --replay-commands "$fixture" \
            --focus --backing "$capture_backing" "$@" "${hashes[@]}" \
            --screenshot "$capture_dir/$name.$pass.png" "$capture_width" "$capture_height" \
            >"$capture_dir/$name.$pass.log" 2>&1; then
            tail -30 "$capture_dir/$name.$pass.log" >&2
            return 1
        fi
        if ! rg -q "$expected" "$capture_dir/$name.$pass.log"; then
            echo "Unexpected HUD state: $name ($pass)" >&2
            tail -20 "$capture_dir/$name.$pass.log" >&2
            return 1
        fi
    done
    rg -q 'determinism: MATCH' "$capture_dir/$name.repeat.log"
    cmp "$capture_dir/$name.first.png" "$capture_dir/$name.repeat.png"
    echo "$name: matching pixels and simulation hashes"
}

capture extractor-upgrade 10 tests/fixtures/hud-extractor-upgrade.commands \
    'hud-state: selected=1 types=1 work=UPGRADING progress=[1-9][0-9]* flow=ACTIVE queued=1' \
    --select-type UEB1103 --look 5410 2772 450
for pass in first repeat; do
    rg -q 'hud-build-option: id=UEB1302 upgrade=1 queued-upgrade=1' \
        "$capture_dir/extractor-upgrade.$pass.log"
    test -s "$capture_dir/extractor-upgrade.$pass.png"
done
capture extractor-queue 10 tests/fixtures/hud-extractor-queue.commands \
    'hud-state: selected=1 types=1 work=UPGRADING progress=[1-9][0-9]* flow=ACTIVE queued=2' \
    --select-type UEB1103 --look 5410 2772 450
for pass in first repeat; do
    rg -q 'production panel: 2 rows, repeat=off' "$capture_dir/extractor-queue.$pass.log"
done
if [[ ${CAPTURE_SCENARIOS:-all} == upgrade ]]; then
    echo "HUD upgrade artifacts: $capture_dir"
    exit 0
fi

capture construction 12 tests/fixtures/hud-construction.commands \
    'hud-state: selected=1 types=1 work=BUILDING progress=[1-9][0-9] flow=ACTIVE queued=0' \
    --select-type UEL0001
capture mixed 22 tests/fixtures/hud-construction.commands \
    'hud-state: selected=3 types=3 work=NONE' --select 3 --look 5400 2772 160
capture production 405 tests/fixtures/hud-production.commands \
    'hud-state: selected=1 types=1 work=BUILDING .*queued=30' --select-type UEB0101 --look 5410 2772 160
capture starved 410 tests/fixtures/hud-production.commands \
    'hud-state: selected=1 types=1 work=BUILDING .*flow=(<1|1)% FUNDED queued=30' --select-type UEB0101 --look 5410 2772 160
# Income still trickles in: this is an energy stall, not a claim of zero progress.
rg -q 'economy: .* / 0 energy,.* [01]% funded' "$capture_dir/starved.first.log"
for scenario in production starved; do
    for pass in first repeat; do
        rg -q 'production panel: 1 rows, repeat=off' "$capture_dir/$scenario.$pass.log"
    done
done
for scenario in construction production starved; do
    rg -q 'construction bars: 1' "$capture_dir/$scenario.first.log"
done
echo "HUD artifacts: $capture_dir"
