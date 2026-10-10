#!/usr/bin/env python3
"""Prepare or privately export the exact external BAR/Recoil/macOS Mesa baseline."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
LOCK_PATH = ROOT / "tools/bar-runtime.lock.json"
LOCK = json.loads(LOCK_PATH.read_text())
FAF = Path.home() / "projects/llm/games/faf"
# Four seconds at Recoil GAME_SPEED=30, after BAR's three-second commander warp
# (common/constants.lua); allow synchronized cheat/devlua commands to arrive.
SMOKE_START_FRAME = 4 * 30


def digest(path: Path) -> str:
    if not path.is_file():
        raise ValueError(f"Missing required file: {path}")
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def tree_digest(root: Path) -> dict:
    if not root.is_dir():
        raise ValueError(f"Missing required directory: {root}")
    value = hashlib.sha256()
    count = 0
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if ".git" in relative.parts or not path.is_file():
            continue
        value.update(str(relative).encode() + b"\0" + digest(path).encode() + b"\n")
        count += 1
    return {"sha256": value.hexdigest(), "files": count}


def require_digest(path: Path, expected: str) -> None:
    actual = digest(path)
    if actual != expected:
        raise ValueError(f"Pinned baseline mismatch: {path}\n  expected {expected}\n  actual   {actual}\nImport the selected baseline; do not substitute an unreviewed engine/library/content build.")


def require_tree(path: Path, expected: dict) -> None:
    actual = tree_digest(path)
    if actual != expected:
        raise ValueError(f"Pinned content tree mismatch: {path}\n  expected {expected}\n  actual   {actual}")


def paths(args: argparse.Namespace) -> dict[str, Path]:
    result = {name: Path(getattr(args, name)).expanduser().resolve() for name in
              ("engine", "mesa", "game", "map", "font", "vulkan_loader", "data_dir")}
    if args.dependencies_dir:
        result["dependencies_dir"] = Path(args.dependencies_dir).expanduser().resolve()
    if args.bundle:
        bundle = Path(args.bundle).expanduser().resolve()
        result.update(engine=bundle / "engine/spring", mesa=bundle / "mesa", game=bundle / "game.sdd",
                      map=bundle / "map" / LOCK["default_map"]["filename"],
                      font=bundle / "font/FreeSansBold.otf", dependencies_dir=bundle / "dependencies",
                      vulkan_loader=bundle / "dependencies/libvulkan.1.dylib")
    return result


def verify(args: argparse.Namespace, selected: dict[str, Path]) -> dict[str, Path]:
    require_digest(selected["engine"], LOCK["engine"]["sha256"])
    if not os.access(selected["engine"], os.X_OK):
        raise ValueError(f"Engine is not executable: {selected['engine']}")
    require_tree(selected["engine"].parent / "base", LOCK["engine"]["base"])
    for name, expected in LOCK["mesa"]["libraries"].items():
        require_digest(selected["mesa"] / "lib" / name, expected)
    require_digest(selected["mesa"] / "share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json", LOCK["mesa"]["icd_sha256"])
    require_digest(selected["font"], LOCK["font"]["sha256"])
    require_tree(selected["game"], LOCK["game"]["tree"])
    dependencies = {}
    for name, specification in LOCK["dependencies"].items():
        source = selected["dependencies_dir"] / name if "dependencies_dir" in selected else Path(specification["path"])
        require_digest(source, specification["sha256"])
        dependencies[name] = source
    require_digest(selected["vulkan_loader"], LOCK["dependencies"]["libvulkan.1.dylib"]["sha256"])
    expected_map = args.map_sha256 or LOCK["default_map"]["sha256"]
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_map):
        raise ValueError("--map-sha256 must be a 64-character SHA-256 checksum")
    require_digest(selected["map"], expected_map.lower())
    if selected["map"].suffix.lower() not in (".sd7", ".sdz"):
        raise ValueError("--map must name a Spring .sd7 or .sdz map archive")
    if not args.map_name or any(c in args.map_name for c in ";{}\n\r\0"):
        raise ValueError("--map-name contains invalid start-script syntax")
    # An engine scans all of a datadir. Keep foreign archives out, so checksum
    # validation cannot be bypassed by an older differently-named game/map.
    for directory, allowed in (("games", "bar.sdd"), ("maps", selected["map"].name)):
        mounted = selected["data_dir"] / directory
        if mounted.exists() and any(p.name != allowed for p in mounted.iterdir()):
            raise ValueError(f"Foreign content in isolated {mounted}; use a fresh --data-dir")
    for content in (selected["game"], selected["engine"].parent / "base", selected["mesa"]):
        if selected["data_dir"] == content or selected["data_dir"].is_relative_to(content):
            raise ValueError(f"Data directory must not modify an external content/runtime tree: {content}")
    return dependencies


def mount(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink():
        if destination.resolve() == source.resolve():
            return
        destination.unlink()
    elif destination.exists():
        raise ValueError(f"Refusing to replace non-launcher mount: {destination}")
    destination.symlink_to(source, target_is_directory=source.is_dir())


def prepare(args: argparse.Namespace, selected: dict[str, Path], dependencies: dict[str, Path]) -> None:
    data = selected["data_dir"]
    data.mkdir(parents=True, exist_ok=True)
    mount(selected["engine"].parent / "base", data / "base")
    mount(selected["game"], data / "games/bar.sdd")
    mount(selected["map"], data / "maps" / selected["map"].name)
    mount(selected["font"], data / "fonts/FreeSansBold.otf")
    for name, source in dependencies.items():
        mount(source, data / "runtime-libs" / name)
    # Mesa's libGL wrapper interposes dlsym and crashes SDL's native Cocoa GL
    # initialization (FAR docs/recoil-macos-rendering.md). Import EGL/driver
    # libraries individually; never put the whole Mesa lib directory on DYLD_*.
    for name in LOCK["mesa"]["libraries"]:
        if name not in ("libGL.dylib", "libgl_interpose.dylib"):
            mount(selected["mesa"] / "lib" / name, data / "runtime-libs" / name)
    # This is runtime configuration, not a change to Mesa or game assets. The
    # donor ICD contains an absolute path, so generate a portable selected ICD.
    icd = json.loads((selected["mesa"] / "share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json").read_text())
    icd["ICD"]["library_path"] = str(selected["mesa"] / "lib/libvulkan_kosmickrisp.dylib")
    (data / "kosmickrisp.json").write_text(json.dumps(icd, indent=2) + "\n")
    settings = data / "springsettings.cfg"
    if not settings.exists():
        settings.write_text("Fullscreen = 0\nXResolution = 1280\nYResolution = 800\nVSync = 1\nShadows = 0\nLuaUI = 1\nFontFile = fonts/FreeSansBold.otf\nSmallFontFile = fonts/FreeSansBold.otf\n")
    debug = f"debugcommands=1:cheat 1|2:devlua 1|{SMOKE_START_FRAME}:runtestsheadless recoil_metal/test_bar_runtime;" if args.smoke else ""
    # NoHelperAIs belongs in MODOPTIONS in this engine. It controls the LuaUI
    # command APIs used by BAR's build/order menus, not just the enemy opponent.
    script = f"""[GAME]
{{
    MapName={args.map_name};
    GameType={LOCK['game']['version']};
    IsHost=1;
    HostIP=127.0.0.1;
    HostPort=8452;
    MyPlayerName=Player;
    MyPlayerNum=0;
    NumPlayers=1;
    NumTeams=2;
    NumAllyTeams=2;
    GameStartDelay=0;
    StartPosType=1;
    FixedRNGSeed=1;
    [MODOPTIONS]
    {{
        NoHelperAIs=0;
        deathmode={"neverend" if args.smoke else "com"};
        {debug}
    }}
    [PLAYER0] {{ Name=Player; Team=0; Spectator=0; }}
    [TEAM0] {{ TeamLeader=0; AllyTeam=0; Side=Armada; RGBColor=0.2 0.55 1.0; Handicap=0; }}
    [TEAM1] {{ TeamLeader=0; AllyTeam=1; Side=Cortex; RGBColor=1.0 0.25 0.2; Handicap=0; }}
    [ALLYTEAM0] {{ NumAllies=0; }}
    [ALLYTEAM1] {{ NumAllies=0; }}
    [AI0] {{ Name=SimpleAI; ShortName=SimpleAI; Team=1; Host=0; IsFromDemo=0; }}
}}
"""
    (data / "skirmish.txt").write_text(script)
    smoke_file = data / "LuaUI/Tests/recoil_metal/test_bar_runtime.lua"
    if args.smoke:
        smoke_file.parent.mkdir(parents=True, exist_ok=True)
        (data / "testlog").mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "src/games/bar/test_bar_runtime.lua", smoke_file)
        # Never let stale results from a previous run masquerade as this run.
        result = data / "testlog/results.json"
        if result.exists():
            result.unlink()
    elif smoke_file.exists():
        smoke_file.unlink()
    receipt = {"baseline_lock_sha256": digest(LOCK_PATH), "engine_sha256": LOCK["engine"]["sha256"],
               "game_tree": LOCK["game"]["tree"], "map_sha256": digest(selected["map"]),
               "paths": {key: str(value) for key, value in selected.items()},
               "opponent": "SimpleAI", "smoke": args.smoke,
               "smoke_test_sha256": digest(smoke_file) if args.smoke else None}
    (data / "baseline.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Pinned BAR baseline mounted in {data}")
    print("Opponent: BAR SimpleAI; human team 0, spectator disabled, LuaUI commands enabled")
    if args.smoke:
        print("Smoke: upstream testlog/results.json, BAR-SMOKE events in infolog.txt, screenshots/; no game content modified")


def export_bundle(args: argparse.Namespace, selected: dict[str, Path], dependencies: dict[str, Path]) -> None:
    destination = Path(args.export_dir).expanduser().resolve()
    if destination.exists():
        raise ValueError(f"Bundle destination already exists: {destination}")
    for source in selected.values():
        if destination == source or destination.is_relative_to(source):
            raise ValueError(f"Bundle destination must be outside its input trees: {source}")
    destination.mkdir(parents=True)
    (destination / "engine").mkdir()
    shutil.copy2(selected["engine"], destination / "engine/spring")
    shutil.copytree(selected["engine"].parent / "base", destination / "engine/base")
    (destination / "mesa/lib").mkdir(parents=True)
    for name in LOCK["mesa"]["libraries"]:
        shutil.copy2(selected["mesa"] / "lib" / name, destination / "mesa/lib" / name)
    (destination / "mesa/share/vulkan/icd.d").mkdir(parents=True)
    shutil.copy2(selected["mesa"] / "share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json",
                 destination / "mesa/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json")
    shutil.copytree(selected["game"], destination / "game.sdd", ignore=shutil.ignore_patterns(".git"))
    (destination / "dependencies").mkdir()
    for name, source in dependencies.items():
        shutil.copy2(source, destination / "dependencies" / name)
    for directory, source, name in (("font", selected["font"], "FreeSansBold.otf"),
                                    ("map", selected["map"], LOCK["default_map"]["filename"])):
        (destination / directory).mkdir()
        shutil.copy2(source, destination / directory / name)
    shutil.copy2(LOCK_PATH, destination / "bar-runtime.lock.json")
    print(f"Exported exact private runtime bundle: {destination}")
    print("This includes external game/map assets. Review per-asset licenses before sharing; do not commit this bundle.")


def fetch_source(args: argparse.Namespace) -> None:
    kind = args.command.removeprefix("fetch-")
    specification = LOCK[kind]
    destination = (ROOT / "build/bar/sources" / kind).resolve()
    url = specification.get("url", specification.get("upstream_url"))
    revision = specification.get("revision", specification.get("upstream_revision"))

    def run(*command: str) -> None:
        subprocess.run(["mise", "exec", "--", *command], check=True)

    def git_output(*command: str) -> str:
        return subprocess.run(["mise", "exec", "--", "git", "-C", str(destination), *command],
                              check=True, capture_output=True, text=True).stdout.strip()

    if destination.exists():
        # Resume interrupted engine source preparation, never reset user work.
        if kind != "engine" or not (destination / ".git").is_dir():
            raise ValueError(f"Source destination already exists: {destination}")
        if git_output("rev-parse", "HEAD") != revision:
            raise ValueError(f"Source destination has a different revision: {destination}")
    else:
        run("git", "init", str(destination))
        run("git", "-C", str(destination), "remote", "add", "origin", url)
        run("git", "-C", str(destination), "fetch", "--depth=256" if kind == "engine" else "--depth=1",
            "origin", revision)
        run("git", "-C", str(destination), "checkout", "--detach", revision)

    if kind == "engine":
        # --depth=1/untagged clones yield a raw hash as Engine.versionMajor.
        # Fetch the actual ancestor release tag and enough history; never
        # override numeric release fields or label this source as the latest.
        tag = specification["numeric_release_tag"]
        run("git", "-C", str(destination), "fetch", "--depth=256", "origin", revision,
            f"refs/tags/{tag}:refs/tags/{tag}")
        if git_output("rev-parse", tag) != specification["numeric_release_tag_object"]:
            raise ValueError(f"Upstream numeric release tag changed: {tag}")
        if git_output("rev-parse", f"{tag}^{{}}") != specification["numeric_release_commit"]:
            raise ValueError(f"Upstream numeric release commit changed: {tag}")
        description = git_output("describe", "--tags", "--abbrev=7", "--candidates=999",
                                 "--match", "[0-9]*", "--long")
        if description != specification["source_git_describe"]:
            raise ValueError(f"Unexpected source-derived engine version: {description}")
        patch = ROOT / specification["port_patch"]
        require_digest(patch, specification["port_patch_sha256"])
        patch_paths = set(re.findall(r"^\+\+\+ b/(.+)$", patch.read_text(), re.MULTILINE))
        changes = set(git_output("diff", "--name-only").splitlines())
        changes.update(git_output("ls-files", "--others", "--exclude-standard").splitlines())
        if changes - patch_paths:
            raise ValueError(f"Unrelated source edits in {destination}: {sorted(changes - patch_paths)}")
        applied = subprocess.run(["mise", "exec", "--", "git", "-C", str(destination),
                                  "apply", "--reverse", "--check", str(patch)],
                                 capture_output=True).returncode == 0
        if not applied:
            run("git", "-C", str(destination), "apply", str(patch))
        configuration = ["-c", "protocol.file.allow=always"]
        if args.engine_cache:
            cache = Path(args.engine_cache).expanduser().resolve()
            if not cache.is_dir():
                raise ValueError(f"Missing source submodule cache: {cache}")
            for row in git_output("config", "--file", ".gitmodules", "--get-regexp",
                                  r"^submodule\..*\.path$").splitlines():
                key, relative = row.split(maxsplit=1)
                source = cache / relative
                if (source / ".git").exists():
                    configuration.extend(["-c", f"{key.removesuffix('.path')}.url={source}"])
        run("git", *configuration, "-C", str(destination), "submodule", "update",
            "--init", "--recursive", "--jobs=4")
        print(f"Source-derived engine version: {description} HEAD; patch {specification['port_patch_sha256']}")
    elif kind == "mesa":
        patch = ROOT / specification["port_patch"]
        require_digest(patch, specification["port_patch_sha256"])
        run("git", "-C", str(destination), "apply", str(patch))
        rgba_patch = ROOT / specification["rgba_patch"]
        require_digest(rgba_patch, specification["rgba_patch_sha256"])
        run("git", "-C", str(destination), "apply", str(rgba_patch))
    elif kind == "game":
        # The captured runnable checkout leaves its developer-only Lua library
        # submodule empty. Fetching it would not reproduce this content tree.
        require_tree(destination, LOCK["game"]["tree"])
    print(f"Fetched selected {kind} source into {destination}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Commands (all project commands go through mise exec --):
  python3 tools/bar_runtime.py prepare [paths]       Verify/import and mount baseline; never starts gameplay
  python3 tools/bar_runtime.py export --export-dir /external/bar-baseline [paths]
  build/recoil-bar --bundle /external/bar-baseline  Import the exact exported baseline and play
  python3 tools/bar_runtime.py fetch-game          Fetch pinned external game to build/bar/sources/game
  python3 tools/bar_runtime.py fetch-engine        Fetch upstream commit plus captured GPL macOS port patch/submodules
  python3 tools/bar_runtime.py fetch-mesa          Fetch pinned macOS Mesa fork

Portable runtime setup: export the verified private baseline once from the existing
installation, transfer that external directory to an Apple Silicon macOS workstation,
then use --bundle. Non-system dylibs are included and verified, and the launcher runs
spring directly with DYLD_LIBRARY_PATH; no SIP-stripped intermediary shell. --engine,
--mesa, --game, --map, --font, --vulkan-loader and --dependencies-dir also accept explicit
paths. Missing or changed binaries/content are fatal; there is no fallback renderer,
no automatic content patch, and no automatic repinning of a new build. This preserves
the compiled artifact hashes, not just a version string. Engine source fetching pins
the actual ancestor numeric release tag and verifies git describe. A tagless shallow
engine build reports a raw hash as versionMajor and is not BAR compatible; never fix
that with a game shim or a fictitious release override. Source patches are recorded
separately from the upstream commit and compiled artifact hashes.

Source build recipe (produces a candidate artifact requiring integration-owner
review of its version, artifact hashes and actual BAR smoke before pinning):
  brew install cmake ninja meson bison llvm molten-vk vulkan-loader sdl3 devil freetype fontconfig minizip libvorbis libogg openal-soft
  python3 -m venv build/bar/mesa-venv
  build/bar/mesa-venv/bin/pip install mako pyyaml packaging
  # Put build/bar/mesa-venv/bin and /opt/homebrew/opt/bison/bin on PATH.
  # Set LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config, use Apple clang.
  meson setup build/bar/mesa-build build/bar/sources/mesa --prefix=$PWD/build/bar/mesa-install -Dbuildtype=release -Dplatforms=macos -Degl-native-platform=surfaceless -Degl=enabled -Dgallium-drivers=zink -Dvulkan-drivers=kosmickrisp -Dglx=disabled -Dgbm=disabled -Dmoltenvk-dir=/opt/homebrew/opt/molten-vk
  ninja -C build/bar/mesa-build install
  # Do not set CI: upstream numeric version validation must remain enabled.
  env -u CI cmake -S build/bar/sources/engine -B build/bar/engine-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_spring-legacy=ON -DBUILD_spring-headless=OFF -DBUILD_spring-dedicated=OFF -DRECOIL_MACOS_SDL3_EGL=ON -DENABLE_STREFLOP=ON -DSDL3_ROOT=/opt/homebrew/opt/sdl3 -DMESA_EGL_ROOT=$PWD/build/bar/mesa-install -DOPENAL_INCLUDE_DIR=/opt/homebrew/opt/openal-soft/include/AL -DOPENAL_LIBRARY=/opt/homebrew/opt/openal-soft/lib/libopenal.dylib
  env -u CI cmake --build build/bar/engine-build --target engine-legacy -j 8
  # fetch-mesa applies the native surface-size fix; unpatched cached-1x1 Mesa is not this baseline.
  # Optional fetch-engine --engine-cache DIR reuses local submodule repositories.
  # STREFLOP_NEON is scoped by upstream/port CMake on ARM64; never add it globally.

A different map requires BOTH --map-name and --map-sha256 (obtain its checksum with
mise exec -- shasum -a 256 /external/map.sd7). The default map is pinned Angel Crossing
1.4; maps and BAR assets remain external. BAR built-in SimpleAI genuinely builds and
orders units but is a weaker opponent than native BARb. Native BARb/CircuitAI support
is not claimed: this build selects the engine target only, not native skirmish AI libraries.
Normal play uses no testing overlay or cheats. --smoke adds an isolated raw LuaUI test
only; upstream BAR Test Runner observes actual move/build effects and AI orders/unit
completion, captures screenshots, writes testlog/results.json and quits. Treat those
results and screenshots as evidence, not a zero engine exit code alone.

Licensing: Recoil/port patch GPL; Mesa/component licenses in upstream licenses/;
FreeFont GPL with font exception; BAR GPL Lua plus mixed asset-specific licenses.
No engine binary, Mesa binary, font, game or map asset is redistributed in this repo.
Private export includes user-provided assets: review their licenses before sharing.
""")
    parser.add_argument("command", choices=("prepare", "export", "fetch-game", "fetch-engine", "fetch-mesa"), nargs="?", default="prepare")
    parser.add_argument("--engine", default=str(ROOT / "build/bar/engine-build/spring"))
    parser.add_argument("--mesa", default=str(ROOT / "build/bar/mesa-install"))
    parser.add_argument("--game", default=str(FAF / "forged-alliance-reborn/reference/BAR"))
    parser.add_argument("--map", default=str(Path.home() / "projects/llm/input/recoil/maps" / LOCK["default_map"]["filename"]))
    parser.add_argument("--map-name", default=LOCK["default_map"]["name"])
    parser.add_argument("--map-sha256")
    parser.add_argument("--font", default=str(FAF / "recoil-macos/cont/fonts/FreeSansBold.otf"))
    parser.add_argument("--vulkan-loader", default="/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib")
    parser.add_argument("--dependencies-dir")
    parser.add_argument("--bundle")
    parser.add_argument("--data-dir", default=str(ROOT / "build/bar"))
    parser.add_argument("--export-dir")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--engine-cache", help="Existing Recoil source checkout; reuse its exact submodule objects without changing it")
    args = parser.parse_args()
    try:
        if args.command.startswith("fetch-"):
            fetch_source(args)
        else:
            selected = paths(args)
            dependencies = verify(args, selected)
            if args.command == "export":
                if not args.export_dir:
                    raise ValueError("export requires --export-dir")
                if args.map_sha256 and args.map_sha256 != LOCK["default_map"]["sha256"]:
                    raise ValueError("Baseline export uses the pinned default map; custom maps remain separately supplied")
                export_bundle(args, selected, dependencies)
            else:
                prepare(args, selected, dependencies)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"BAR runtime preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
