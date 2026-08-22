#!/usr/bin/env bash
#
# Fetch the AI corpora the adapters are written against (ADR-039), into vendor/ai/.
#
# WHY A SCRIPT RATHER THAN A COMMIT. These trees are 16 MB and 1,016 files of other people's
# code — committing them would bury this project's own under someone else's and make "what did
# we write" unanswerable, which is the reason .gitignore already gives for third_party/. And
# the whole point of adapting these AIs rather than reimplementing them is that their
# communities keep improving them: a pinned fetch makes "FAF shipped something better" a
# one-line change to this file, while a committed copy makes it a 16 MB diff nobody reviews.
#
# WHY PINNED RATHER THAN A BRANCH TIP. An adapter is written against particular semantics. If
# `develop` moves under it, the adapter breaks for a reason that is not in this repository's
# history. The pins below are the versions every measurement in ADR-039 was taken on.
#
# THE RULE THESE TREES LIVE UNDER: nothing under vendor/ai/ is ever modified. See
# vendor/README.md. A pinned fetch enforces that by construction — a local edit is silently
# discarded by the next --force, which is the correct outcome.
#
#   tools/fetch_ai.sh                  fetch what is missing, leave what is present
#   tools/fetch_ai.sh --force          re-fetch everything, asking before it deletes anything
#   tools/fetch_ai.sh --force --yes    the same without the prompt, for scripts
#   make ai [FORCE=1] [YES=1]          the same, from the Makefile
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$ROOT/vendor/ai"

FORCE=0
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --yes)   ASSUME_YES=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# --- The pins ----------------------------------------------------------------
#
# Forged Alliance Forever's game AI. A 2.5 GB repository of which we need 4.7 MB, so this is a
# blobless sparse checkout rather than a clone — see fetch_sparse below.
FAF_URL='https://github.com/FAForever/fa.git'
FAF_SHA='82a2612d083deafb6bec99fc85a3f1aa6cbcfedb'   # 2026-08-14, on origin/develop
#
# Patterns are gitignore-style (sparse-checkout --no-cone), so each is anchored with a leading
# slash: a bare `engine` would match any directory of that name at any depth.
FAF_PATHS=(
    /lua/AI                             # builders, base templates, OpAI, platoon templates
    /lua/aibrains                       # FAF's modern rewrite: adaptive/rush/tech/turtle
    /lua/aibrain.lua                    # the brain object
    /lua/aibrainPlans.lua
    /lua/aipersonality.lua
    /lua/factions.lua                   # faction table: GetFactions drives the factory manager
    /lua/platoon.lua                    # the platoon behaviour library
    /lua/sim/Builder.lua                # the manager classes the brain drives
    /lua/sim/BuilderManager.lua
    /lua/sim/EngineerManager.lua
    /lua/sim/FactoryBuilderManager.lua
    /lua/sim/PlatoonFormManager.lua
    /lua/sim/BrainConditionsMonitor.lua
    /lua/system                         # Class/ClassSimple and friends: the corpus's own OO
                                        # helper, which every manager and platoon is built with
    /engine                             # LuaLS annotation stubs for Moho — the binding spec
)

# CircuitAI, which Beyond All Reason ships as "BARb". Small enough to take whole, and taking it
# whole is also correct: trimming an upstream tree is a fork by subtraction.
CIRCUIT_URL='https://github.com/rlcevg/CircuitAI.git'
CIRCUIT_SHA='0ef36267633d6c1b2f6408a8d8a59fff38745dc3'   # 2025-12-17

# The C ABI CircuitAI is compiled against. It lives in the engine, not in the AI, and without it
# CircuitAI does not build.
#
# Resolved 2026-08-21 by fetching master and checking what came back: the Interface headers are
# byte-identical to the local checkout every measurement in ADR-039 was taken on, so this pin
# names the same 596 callbacks, 27 events and 109 command topics. (AI/Wrappers differs in three
# files and drops JavaOO — that difference is in the LOCAL checkout, which carries four macOS
# build commits on top of an unversioned import. Upstream is the clean one; this is it.)
RECOIL_URL='https://github.com/beyond-all-reason/RecoilEngine.git'
RECOIL_SHA='bcdca85760da1341a73f042be3ed3f7848d6b157'   # master as of 2026-08-21
RECOIL_PATHS=(
    /AI/Wrappers                        # the C++ OO wrapper (mostly awk-generated at build time)
    /rts/ExternalAI/Interface           # SSkirmishAICallback.h, AISEvents.h, AISCommands.h
)

# --- Fetching ----------------------------------------------------------------
#
# `git clone --depth 1` takes a branch tip, not a commit, so a pinned shallow fetch has to be
# spelled out: init, add the remote, fetch that one commit, check it out. Sparse checkout keeps
# a 2.5 GB repository from landing on disk to extract 4.7 MB of it.

# Empty a directory, ASKING FIRST. `--force` exists to discard a local tree, which is the
# correct outcome for a vendored corpus nobody should have edited — but "correct outcome" and
# "silently deleted without being told" are different things, and a mistyped --force should not
# cost work. `--yes` skips the prompt for scripted use.
#
# Not `rm -rf`, which this machine's tooling declines to run. git's object store is mode 444, so
# u+w first or the delete fails inside .git/objects.
wipe() {
    local dir="$1"
    [[ -d "$dir" ]] || return 0

    if [[ $ASSUME_YES -eq 0 ]]; then
        local n
        n="$(find "$dir" -type f | wc -l | tr -d ' ')"
        echo
        echo "  About to DELETE $dir ($n files)."
        read -r -p "  Type yes to continue: " reply
        [[ "$reply" == "yes" ]] || { echo "  Aborted."; exit 1; }
    fi

    chmod -R u+w "$dir"
    find "$dir" -mindepth 1 -delete
}

fetch_sparse() {
    local name="$1" url="$2" sha="$3"; shift 3
    local paths=("$@") dir="$DST/$name"

    if [[ -d "$dir" && $FORCE -eq 0 ]]; then
        echo "  $name: present, skipping (--force to re-fetch)"
        return
    fi
    wipe "$dir"

    echo "  $name: fetching $sha from $url"
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    # `sparse-checkout set` sets core.sparseCheckout and writes the patterns itself; a separate
    # `init` is both unnecessary and, on git 2.49, an error (it takes neither -q nor --no-cone).
    # Patterns go in BEFORE the fetch so --filter=blob:none never downloads what is excluded.
    git -C "$dir" sparse-checkout set --no-cone -- "${paths[@]}"
    git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
    echo "  $name: $(git -C "$dir" rev-parse --short HEAD), $(find "$dir" -type f -not -path '*/.git/*' | wc -l | tr -d ' ') files"
}

fetch_whole() {
    # Two statements, not one: bash expands every word of a `local` before performing any of its
    # assignments, so `dir="$DST/$name"` on the same line would read $name while still unset.
    local name="$1" url="$2" sha="$3"
    local dir="$DST/$name"

    if [[ -d "$dir" && $FORCE -eq 0 ]]; then
        echo "  $name: present, skipping (--force to re-fetch)"
        return
    fi
    wipe "$dir"

    echo "  $name: fetching $sha from $url"
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
    echo "  $name: $(git -C "$dir" rev-parse --short HEAD), $(find "$dir" -type f -not -path '*/.git/*' | wc -l | tr -d ' ') files"
}

mkdir -p "$DST"
echo "Fetching AI corpora into vendor/ai/ (see vendor/NOTICE.md for licences)"
fetch_sparse faf     "$FAF_URL"     "$FAF_SHA"     "${FAF_PATHS[@]}"
fetch_whole  circuit "$CIRCUIT_URL" "$CIRCUIT_SHA"
fetch_sparse recoil  "$RECOIL_URL"  "$RECOIL_SHA" "${RECOIL_PATHS[@]}"

echo
echo "Done. vendor/ai/ is gitignored and is never edited by hand — see vendor/README.md."
