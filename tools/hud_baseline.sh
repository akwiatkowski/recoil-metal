#!/usr/bin/env bash
# HUD slice 0: the visual and GPU baseline of the native interface (item recoil-metal-3628).
#
# Captures seven interface states at four logical-size / backing-scale points, benchmarks each
# with and without the HUD, and writes everything OUTSIDE the repo: PNGs, a CSV of logical size,
# backing scale, HUD vertex counts per layer and GPU frame time, contact sheets per size, and a
# REPORT.md with the exact commands. Every capture is headless and deterministic, so a later run
# diffs against this one image for image.
#
# Usage:  tools/hud_baseline.sh [OUT_DIR]
#   FA_ROOT   the retail install (default: the external volume's)
#   FA_UNITS  the extracted units tree with URL0105 (default: ~/projects/llm/input/faf/units)
#   FRAMES    benchmark frames per point (default 300; the first 60 are warmup)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/build/recoil-metal"
FA_ROOT="${FA_ROOT:-/Volumes/Samsung_T5/faf/Supreme Commander Forged Alliance}"
FA_UNITS="${FA_UNITS:-$HOME/projects/llm/input/faf/units}"
FRAMES="${FRAMES:-300}"
OUT="${1:-$HOME/projects/llm/input/recoil-metal/hud-baseline/$(date +%F)}"
MAP="$FA_ROOT/maps/SCMP_009/SCMP_009.scmap"
GAMEDATA="$FA_ROOT/gamedata"

[ -x "$BIN" ] || { echo "build first: $BIN" >&2; exit 1; }
[ -f "$MAP" ] || { echo "no retail map at $MAP (set FA_ROOT)" >&2; exit 1; }
mkdir -p "$OUT/shots" "$OUT/bench"

# The skirmish at five minutes: a base under construction, a stall on the economy panel, and
# the player's commander to select. `--no-interpolate` because every golden image is taken
# with it (README, "Every flag").
SKIRMISH=("$MAP" --gamedata "$GAMEDATA" --skirmish --armies 2 --play 300 --focus --no-interpolate)
ENGINEER=("$MAP" --gamedata "$GAMEDATA" --units "$FA_UNITS/URL0105/URL0105_unit.bp" 1 --focus --no-interpolate)

# Where to put the placement ghost: beside the commander the skirmish selects, so the ghost is
# inside the framed shot. Probed once, headless, from the capture's own "ring N at (x, z)" line.
probe="$("$BIN" "${SKIRMISH[@]}" --select 1 --screenshot "$OUT/shots/probe.png" 320 180 2>&1 || true)"
site="$(printf '%s\n' "$probe" | sed -n 's/^ *ring 1 at (\([-0-9.]*\), \([-0-9.]*\)).*/\1 \2/p' | head -1)"
if [ -z "$site" ]; then site="0 0"; fi
GX="$(awk -v s="$site" 'BEGIN{split(s,a," "); printf "%.0f", a[1]+40}')"
GZ="$(awk -v s="$site" 'BEGIN{split(s,a," "); printf "%.0f", a[2]}')"
rm -f "$OUT/shots/probe.png"

# state name | extra arguments (the scene is the skirmish unless the state says otherwise)
STATES=(
  "default|"
  "commander|--select 1"
  "selection|--select 6"
  "engineer|ENGINEER --select 1"
  "placement|--select 1 --hover 1 --ghost $GX $GZ"
  "observer|--observer"
  "faf|--select 1 --ui faf"
)
# logical width, logical height, backing scale, label
SIZES=(
  "1280 720 1 hd-1x"
  "1600 900 1 hd+-1x"
  "1512 982 2 mbp14-2x"
  "2560 1440 2 5k-2x"
)

CSV="$OUT/baseline.csv"
echo "state,size,logical_w,logical_h,backing,pixels_w,pixels_h,hud_scale,world_overlay,panel_surface,chrome,icon,label,readout,gpu_ms_hud,gpu_p95_hud,gpu_ms_world,gpu_p95_world,hud_ms" > "$CSV"
CMDS="$OUT/commands.sh"
{ echo "#!/usr/bin/env bash"; echo "# every command this baseline ran, in order"; echo "BIN=$BIN"; } > "$CMDS"

parse_hud() {  # stdin: program output -> six per-layer uploaded counts, comma separated
  sed -n 's/.*hud vertices.*world-overlay=\([0-9]*\)\/.* panel-surface=\([0-9]*\)\/.* chrome=\([0-9]*\)\/.* icon=\([0-9]*\)\/.* label=\([0-9]*\)\/.* readout=\([0-9]*\)\/.*/\1,\2,\3,\4,\5,\6/p' | head -1
}
parse_gpu() {  # stdin -> "mean,p95"
  sed -n 's/.*gpu mean \([0-9.]*\) ms p95 \([0-9.]*\).*/\1,\2/p' | head -1
}

for size in "${SIZES[@]}"; do
  read -r W H B LABEL <<<"$size"
  for entry in "${STATES[@]}"; do
    name="${entry%%|*}"; extra="${entry#*|}"
    if [[ "$extra" == ENGINEER* ]]; then scene=("${ENGINEER[@]}"); extra="${extra#ENGINEER}"; else scene=("${SKIRMISH[@]}"); fi
    # shellcheck disable=SC2206
    extraArr=($extra)  # may be empty; expanded with the ${a[@]+...} idiom for bash 3.2 under -u
    png="$OUT/shots/$name-$LABEL.png"
    shot=("$BIN" "${scene[@]}" ${extraArr[@]+"${extraArr[@]}"} --screenshot "$png" "$W" "$H" --backing "$B")
    printf '%q ' "${shot[@]}" >> "$CMDS"; echo >> "$CMDS"
    out="$("${shot[@]}" 2>&1)"
    pixels="$(printf '%s\n' "$out" | sed -n 's/.*-> \([0-9]*\)x\([0-9]*\) pixels, hud scale \([0-9.]*\).*/\1,\2,\3/p' | head -1)"
    hud="$(printf '%s\n' "$out" | parse_hud)"
    [ -n "$hud" ] || hud="0,0,0,0,0,0"

    csvHud="$OUT/bench/$name-$LABEL-hud.csv"
    csvWorld="$OUT/bench/$name-$LABEL-world.csv"
    benchHud=("$BIN" "${scene[@]}" ${extraArr[@]+"${extraArr[@]}"} --bench-offscreen "$FRAMES" "$csvHud" --bench-size "$W" "$H" --backing "$B" --bench-hud)
    benchWorld=("$BIN" "${scene[@]}" ${extraArr[@]+"${extraArr[@]}"} --bench-offscreen "$FRAMES" "$csvWorld" --bench-size "$W" "$H" --backing "$B")
    printf '%q ' "${benchHud[@]}" >> "$CMDS"; echo >> "$CMDS"
    printf '%q ' "${benchWorld[@]}" >> "$CMDS"; echo >> "$CMDS"
    gpuHud="$("${benchHud[@]}" 2>&1 | parse_gpu)"
    gpuWorld="$("${benchWorld[@]}" 2>&1 | parse_gpu)"
    hudMs="$(awk -v a="${gpuHud%%,*}" -v b="${gpuWorld%%,*}" 'BEGIN{printf "%.3f", a-b}')"
    echo "$name,$LABEL,$W,$H,$B,$pixels,$hud,$gpuHud,$gpuWorld,$hudMs" >> "$CSV"
    echo "  $name @ $LABEL: hud $hud | gpu hud ${gpuHud%%,*} ms, world ${gpuWorld%%,*} ms"
  done
  # One contact sheet per size point, labelled, downscaled to a browsable width.
  files=()
  for entry in "${STATES[@]}"; do files+=("$OUT/shots/${entry%%|*}-$LABEL.png"); done
  python3 "$REPO/tools/hud_contact_sheet.py" "$OUT/contact-$LABEL.jpg" 640 "${files[@]}"
done

{
  echo "# HUD baseline — $(date +%F)"
  echo
  echo "Repo: $(git -C "$REPO" rev-parse --short HEAD) · map: SCMP_009 · frames per benchmark: $FRAMES (60 warmup) · machine: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)"
  echo
  echo "Every row is one headless run: the capture is \`--screenshot W H --backing S\`, the GPU"
  echo "numbers are \`--bench-offscreen\` at the same logical size and backing, once with"
  echo "\`--bench-hud\` (the interface the capture shows) and once world-only; \`hud_ms\` is the"
  echo "difference of the two means. Commands are in \`commands.sh\`, images in \`shots/\`,"
  echo "per-frame timings in \`bench/\`, contact sheets are \`contact-*.jpg\`."
  echo
  echo '| state | size | logical | backing | pixels | HUD scale | HUD vertices (surface/chrome/icon/label/readout) | gpu HUD ms | gpu world ms | HUD ms |'
  echo '|---|---|---|---|---|---|---|---|---|---|'
  tail -n +2 "$CSV" | awk -F, '{printf "| %s | %s | %sx%s | %s | %sx%s | %s | %s/%s/%s/%s/%s | %s | %s | %s |\n", $1,$2,$3,$4,$5,$6,$7,$8,$10,$11,$12,$13,$14,$15,$17,$19}'
} > "$OUT/REPORT.md"

echo "baseline written to $OUT"
