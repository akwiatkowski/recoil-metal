<!-- Generated and maintained by Claude -->
# recoil-metal

A Mac-native, Metal-only renderer that reads **Recoil** content formats —
research spun out of [FAR](../forged-alliance-reborn/README.md), where the
whole project is blocked because Recoil's model path needs OpenGL 4.3+ and
Apple caps OpenGL at 4.1.

The research question:

> Can a purpose-built Metal renderer load real Recoil content (SMF maps first,
> units later) and beat Recoil's OpenGL path on Apple Silicon — and what does
> a modern C++ engine core look like when built test-first, without 20 years
> of legacy coupling?

This is a personal research project. It is also a deliberate modern-C++
showcase: C++23, RAII everywhere, rule of five, pImpl at platform boundaries,
TDD with Catch2, warnings-as-errors.

## Why not port Recoil class-by-class

Measured on the real tree (`reference/RecoilEngine` in FAR, Spring/Recoil
`rts/`): of ~1.95M lines, **1.57M is `rts/lib/`** — vendored third-party code
nobody rewrites. The engine proper is ~385k lines:

| Subsystem | kLOC | Plan |
|---|---|---|
| Sim (units, weapons, pathing) | 83k | later, *semantics* reimplemented — not classes |
| Lua bindings | 74k | never port; reimplement minimal when needed |
| Rendering (OpenGL) | 67k | **replaced — this project** |
| System (SDL, threads, FS) | 65k | take only what's needed |
| Game / ExternalAI / RmlUI / Net / Menu | ~81k | never port |

Two facts make the strategy possible: `rts/Sim` includes **zero** GL headers,
and only 18 sim files touch rendering globals — the sim↔rendering boundary is
real. But a horizontal port (Sim → Lua → Game → System) still means ~250k
lines before one pixel renders. So instead: **vertical slices through data
formats**. Loaders are small and data-shaped; the renderer is new.

The rule, inherited from FAR: *assets/formats convert, behaviour gets
reimplemented.*

## Milestones

1. **Window + runtime-compiled shader.** NSWindow, `CAMetalLayer`, vsync via
   `CAMetalDisplayLink`, animated clear colour. The shader pipeline is built
   from *source at runtime* — there is no Xcode on this machine, hence no
   offline `metal` compiler. This milestone exists to prove that toolchain
   story holds. **← current**
2. **SMF terrain.** Port the SMF/SMT map loader from Recoil (`rts/Map/SMF/`,
   ~10k lines total in `rts/Map/`, the SMF part much smaller), heightmap mesh,
   solid shading, orbit camera. TDD: the parser is test-first against real
   `.smf` files from FAR's reference tree.
3. **Textured terrain.** SMT tile decoding (DDS), texture arrays.
4. **Benchmark harness.** Frame-time capture to CSV; same map in Recoil GL
   vs recoil-metal. This is where the research question gets its answer.
5. **Units.** glTF/S3O loading, instanced rendering, skinning (Recoil's
   vertex format already carries bone IDs/weights — see FAR's docs).

Stage B (sim semantics, only if milestones 1–5 prove out) is deliberately not
planned. The cliff is real; plan when we're on it.

## Build

Requires: CMake + Ninja (brew), Apple clang (CLT — no Xcode needed), macOS 14+.

```sh
# One-time dependency: Apple's official C++ bindings for Metal.
git clone --depth 1 https://github.com/apple/metal-cpp third_party/metal-cpp

mise exec -- cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
mise exec -- cmake --build build
mise exec -- ctest --test-dir build --output-on-failure
./build/recoil-metal
```

Catch2 is fetched by CMake at configure time.

## Layout

```
recoil-metal/
├── CMakeLists.txt      single build file, sections commented
├── src/
│   ├── core/           pure C++ — no Metal, no AppKit, fully unit-tested
│   ├── render/         Metal renderer (Objective-C++ where bridging)
│   ├── platform/       AppKit window + display link (pImpl hides ObjC)
│   └── main.mm         thin entry point
├── tests/              Catch2 unit tests, mirrors src/core
├── third_party/        metal-cpp (git-cloned, gitignored)
└── docs/               research notes, benchmark results
```

## Legal

Recoil is GPL-2.0; loader code adapted from it makes this project GPL-2.0 as
well. Reading Recoil source as a spec is unrestricted. No game assets are or
will ever be committed — same rule as FAR.
