#!/usr/bin/env bash
#
# Recoil GL baseline for milestone 4: renders the SAME map recoil-metal renders,
# through Recoil's OpenGL path, and captures per-frame times to CSV.
#
# The chain being measured (FAR's work, see its docs/recoil-macos-rendering.md):
#
#     Recoil (OpenGL 4.6 compat)
#       -> SDL3 + CAMetalLayer
#       -> Mesa EGL (surfaceless)
#       -> zink            OpenGL -> Vulkan
#       -> kosmickrisp     Vulkan -> Metal
#       -> Apple M4 Pro
#
# What is compared, and why only this: Recoil exposes no GPU timer to Lua, so the
# only quantity available on both sides is wall-clock frame period. VSync is
# therefore forced OFF here, and recoil-metal's comparable number comes from
# `--bench-offscreen` (also unthrottled). Comparing against recoil-metal's
# windowed `--bench` would compare two display refresh rates, not two renderers.
#
# Terrain only on both sides: this uses tools/rmbench.sdd, a minimal game with no
# units, rather than FAR's converted content.
#
# Usage: tools/bench_recoil_gl.sh [out.csv]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FAR_ROOT="${HOME}/projects/llm/games/forged-alliance-reborn"

ENGINE="${ENGINE:-${FAR_ROOT}/build/gl-sdl3-runtime/spring}"
MESA="${MESA:-${HOME}/projects/llm/games/mesa-macos/install}"
MAP_SD7="${MAP_SD7:-${HOME}/projects/llm/input/recoil/maps/angel_crossing_1.4.sd7}"
MAP_NAME="${MAP_NAME:-Angel Crossing 1.4}"
OUT_CSV="${1:-${REPO_ROOT}/docs/bench-recoil-gl-aw04.csv}"

DATA_DIR="${REPO_ROOT}/build/gl-bench-datadir"
LOG="${DATA_DIR}/run.log"

for path in "${ENGINE}" "${MAP_SD7}"; do
	if [[ ! -f "${path}" ]]; then
		echo "missing: ${path}" >&2
		exit 1
	fi
done
if [[ ! -d "${MESA}/lib" ]]; then
	echo "no Mesa install at ${MESA}" >&2
	exit 1
fi

# --- Data directory -------------------------------------------------------
# Self-contained so this does not disturb FAR's datadir, but the engine's own
# base content and font come from the rescued runtime.
mkdir -p "${DATA_DIR}/maps" "${DATA_DIR}/games" "${DATA_DIR}/fonts"
cp -R "$(dirname "${ENGINE}")/base" "${DATA_DIR}/" 2>/dev/null || true
cp -R "${REPO_ROOT}/tools/rmbench.sdd" "${DATA_DIR}/games/"
cp "${MAP_SD7}" "${DATA_DIR}/maps/"

FONT_SRC="${HOME}/projects/llm/games/recoil-macos/cont/fonts/FreeSansBold.otf"
if [[ -f "${FONT_SRC}" ]]; then
	cp "${FONT_SRC}" "${DATA_DIR}/fonts/" 2>/dev/null || true
fi

# --- Settings -------------------------------------------------------------
# VSync=0 is the whole point: with it on, every frame time is the display period
# and the comparison measures the monitor.
cat > "${DATA_DIR}/springsettings.cfg" <<'CFG'
VSync = 0
XResolution = 1920
YResolution = 1080
Fullscreen = 0
FPSFOV = 0
Shadows = 0
CFG

# --- Start script ---------------------------------------------------------
cat > "${DATA_DIR}/bench.txt" <<SCRIPT
[GAME]
{
	Mapname=${MAP_NAME};
	GameType=rmbench;
	StartPosType=1;
	NumPlayers=1;
	NumTeams=1;
	NumAllyTeams=1;
	HostIP=127.0.0.1;
	HostPort=8452;
	MyPlayerName=bench;
	IsHost=1;

	[PLAYER0]
	{
		Name=bench;
		Team=0;
		Spectator=1;
	}

	[TEAM0]
	{
		TeamLeader=0;
		AllyTeam=0;
		RGBColor=1.0 0.2 0.2;
		Side=;
		Handicap=0;
	}

	[ALLYTEAM0]
	{
		NumAllies=0;
	}
}
SCRIPT

# --- The environment the zink chain needs ---------------------------------
# Every one of these is load-bearing; see FAR's tools/run_render_test.sh for the
# full reasoning behind each.
export VK_DRIVER_FILES="${MESA}/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"
export MESA_VULKAN_LIBRARY="/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib"
export EGL_PLATFORM="surfaceless"
export MESA_LOADER_DRIVER_OVERRIDE="zink"
export MESA_GL_VERSION_OVERRIDE="4.6COMPAT"
export MESA_GLSL_VERSION_OVERRIDE="460"
export SPRING_DATADIR="${DATA_DIR}"
export SDL_AUDIODRIVER="dummy"

echo "engine: ${ENGINE}"
echo "map:    ${MAP_NAME}"
echo "--- running (vsync off) ---"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-240}"
# Not via a system binary with DYLD_* set — SIP strips those, which is exactly
# what MESA_VULKAN_LIBRARY exists to work around.
perl -e 'alarm shift; exec @ARGV' "${TIMEOUT_SECONDS}" \
	"${ENGINE}" "${DATA_DIR}/bench.txt" > "${LOG}" 2>&1 || true

# --- Extract the CSV ------------------------------------------------------
# Only the RMBENCH sample lines, and only the i:ms tokens on them. A looser
# pattern picks up the log's own "[t=00:00:43.81]" timestamps and silently
# inflates the row count — the first version of this did exactly that. Note the
# pattern is unanchored: Recoil prefixes every log line with [t=..][f=..].
mkdir -p "$(dirname "${OUT_CSV}")"
{
	echo "frame,cpu_ms,gpu_ms"
	# gpu_ms is 0: Recoil exposes no GPU timer to Lua. The column exists so both
	# CSVs share a schema; that it is always zero is stated, not implied.
	grep 'RMBENCH ' "${LOG}" \
		| grep -v 'RMBENCH-DONE' \
		| tr ' ' '\n' \
		| grep -E '^[0-9]+:[0-9.]+$' \
		| awk -F: '{ printf "%d,%s,0.0000\n", $1 - 1, $2 }'
} > "${OUT_CSV}"

ROWS=$(( $(wc -l < "${OUT_CSV}") - 1 ))
echo "captured ${ROWS} frames -> ${OUT_CSV}"

if [[ "${ROWS}" -le 0 ]]; then
	echo "no frame timings captured; tail of the log:" >&2
	tail -30 "${LOG}" >&2
	exit 1
fi

# macOS awk has no asort, so percentiles come from sort(1).
MEAN=$(awk -F, 'NR>1 { s+=$2 } END { printf "%.4f", s/(NR-1) }' "${OUT_CSV}")
awk -F, 'NR>1 { print $2 }' "${OUT_CSV}" | sort -n \
	| awk -v mean="${MEAN}" '{ a[NR]=$1 } END { printf "recoil-gl: %d frames | cpu mean %s ms p50 %.3f p95 %.3f p99 %.3f max %.3f | %.0f fps\n", NR, mean, a[int(NR*0.5)], a[int(NR*0.95)], a[int(NR*0.99)], a[NR], 1000/mean }'
