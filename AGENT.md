<!-- Generated and maintained by Claude -->
# AGENT.md — working rules for recoil-metal

Read this before touching anything. [`README.md`](README.md) explains *what
and why*; this file is *how to work here* and *what's already decided*.

**Project in one line:** a Metal-only, Mac-native renderer for Recoil content
formats, built test-first in modern C++, as both research and a C++ showcase.

---

## Hard rules

1. **Recoil source is read-only reference.** It lives at
   `../forged-alliance-reborn/reference/RecoilEngine` (owned by the FAR
   project — never edit it, never re-clone it from here). Adapting its loader
   code into this repo is allowed and expected; that's why this repo is
   GPL-2.0.
2. **Use `mise exec --` for every tool invocation** (`mise exec -- cmake …`,
   `mise exec -- ctest …`). Never call tools directly.
3. **Never commit game assets or `third_party/` content.** Converted or
   loaded game data stays in gitignored build directories.
4. **TDD is not optional for anything pure.** If it doesn't touch the GPU,
   it gets a failing test first. Rendering itself is verified on screen —
   "parses" is not "animates", same rule as FAR.
5. **Don't re-litigate settled decisions** (below) without a concrete new fact.

## Settled decisions

| Decision | Value | Why |
|---|---|---|
| Language | **C++23** (clang) | Showcase goal; downgrade to C++20 only if a feature breaks the build |
| GPU API | **Metal via metal-cpp**, Apple's official zero-overhead C++ binding | The whole point; also the only way around Apple's GL 4.1 cap |
| Portability | **macOS-only, deliberately** | No abstraction layers "just in case" — YAGNI, documented |
| Shader toolchain | **Runtime compilation from source** | No Xcode on this machine → no offline `metal` compiler; also enables hot-reload later |
| Windowing | **AppKit + CAMetalDisplayLink**, no SDL/GLFW | Mac-native means Mac-native; dependencies stay near zero |
| Test framework | **Catch2 v3**, FetchContent at configure time | Standard, BDD-friendly, good matchers |
| Strategy | **Vertical slices through data formats** | See README — horizontal port of Recoil is ~250k lines before a pixel |
| Licence | **GPL-2.0** | We adapt Recoil (GPLv2) loader code |

## C++ style — this repo is a showcase

- **RAII for every resource.** Metal objects are released in destructors in
  reverse acquisition order. No raw `new`, no leaks-by-convention.
- **Rule of five, explicitly.** Every owning class either defines or
  `= delete`s all five special members. No silent accidental copies of GPU
  resources.
- **pImpl at platform boundaries.** Public headers never expose Objective-C
  or Metal types, so `core/` compiles (and tests) without a single framework.
- **`[[nodiscard]]`, `noexcept`, `constexpr`** — used correctly, not
  sprinkled. `noexcept` only where a throw would be a bug (e.g. per-frame
  draw calls); startup failures *do* throw.
- **Specific exception types** (`core/Error.hpp`) with actionable messages.
  No catch-alls, no swallowed errors.
- **No magic numbers.** Every constant carries a comment with its source or
  rationale — same rule as FAR.
- **Comments are a learning tool here.** Explain *why*, cite sources for
  format details and API behaviour (header file, Apple doc, Recoil file:line).

## Verifying work

- `mise exec -- ctest --test-dir build --output-on-failure` must be green
  before any task is called done.
- Parsers are tested against **real files** from the FAR reference tree —
  never hand-crafted fixtures when the real thing is on disk.
- Anything visual must run in the app. A screenshot or it didn't happen.

## Gotchas (grow this list)

- **metal-cpp is not ARC.** C++ objects from `alloc()->init()` and
  `newXxx()` are +1 and must be `->release()`d; `nextDrawable()` and
  `commandBuffer()` are autoreleased. Get this wrong and you leak GPU memory
  per frame.
- The `NS_PRIVATE_IMPLEMENTATION` / `CA_PRIVATE_IMPLEMENTATION` /
  `MTL_PRIVATE_IMPLEMENTATION` defines may appear in **exactly one**
  translation unit (currently `src/render/Renderer.mm`).
- metal-cpp types are layout-compatible with their Objective-C twins by
  design, so `reinterpret_cast<CA::MetalLayer*>(aCAMetalLayer)` is sanctioned
  — but say so in a comment wherever it happens.
- `CAMetalDisplayLink` requires macOS 14+. Fine (target machine is current),
  but don't let anyone "fix" it back to CVDisplayLink without a reason.
- **With `CAMetalDisplayLink`, never call `nextDrawable()` yourself** — the
  link owns the drawable pool and throws `CAMetalLayerInvalidOperation`.
  Take the drawable from the `CAMetalDisplayLinkUpdate` instead. (Learned
  the hard way at milestone 1: it compiles fine and crashes at runtime.)
- **The `.smf` binary header is NOT the authority on vertical scale.**
  `mapinfo.lua`'s `smf.minheight`/`smf.maxheight` override it entirely
  (`MapInfo.cpp:405-418` → `SMFReadMap.cpp:133-158`). BAR's Angel Crossing 1.4
  ships a header saying `minHeight=850, maxHeight=-150` — inverted — and fixes
  it in Lua. Honour only the header and the map renders **upside down**: every
  hill becomes a pit, silently and plausibly. Found at milestone 2 by testing
  against a real map; no synthetic fixture would ever have shown it.
- **Height decode divides by 65536, not 65535.** `0xFFFF` never quite reaches
  `maxHeight`. Getting this wrong is a fraction of a percent of error — small
  enough to look fine and never be noticed.
- **`std::from_chars` for floating point is still deleted in Apple's libc++.**
  Use `strtof`. The integer overloads are fine.
- **BC1/DXT1 is native on Apple Silicon** (`MTLDevice::supportsBCTextureCompression`
  is true on an M4 Pro), so `.smt` tiles go to the GPU verbatim. Recoil's ETC1
  transcode pass for drivers without S3TC (`SMFGroundTextures.cpp:290-317`) has
  no counterpart here — do not port it.
- **A map has far more tiles than Metal allows array layers** (65 536 vs 2 048),
  so the ground texture is an *atlas*, not a texture array. It works out because
  tiles tile the map exactly at 1 texel per elmo.
- **`Nil` and `Boolean` are taken by system headers** (`<objc/objc.h>`,
  `<MacTypes.h>`). Even scoped enumerators lose to the preprocessor, so any
  header that might be included from a `.mm` must avoid those spellings.
- **Do not test binary payloads by counting a sentinel byte.** `0xAA` is the
  engine's missing-tile fill *and* an entirely ordinary DXT1 selector byte
  (`0b10101010`) — it occurs in ~2.8% of real tile data. Assert exact placement
  against the source bytes instead.
- **A unit's facing is `atan2(dx, dz)`, not `atan2(dz, dx)`.** The vertex shader
  maps a model's local +Z to `(sin yaw, cos yaw)`, so yaw is measured from +Z
  toward +X. Swapping the arguments compiles, runs, and renders every unit
  walking sideways — which reads as a model-orientation bug, not a maths one.
- **`setUnits` reorders batches by texture pair**, so the caller's batch index
  is NOT the renderer's slot index, and empty batches are skipped entirely.
  Anything addressing a batch after upload (`setInstances`) must go through the
  mapping the renderer keeps, or it silently drives the wrong model.
- **Instance data is `StorageModeShared`, so per-frame writes need a ring AND a
  fence.** Overwriting the buffer while the GPU may still be reading last
  frame's copy tears the transform of whatever unit is being written — it shows
  up as a single unit at the origin for one frame, which looks exactly like a
  fluke worth ignoring. `beginFrame` waits on a `kMaxFramesInFlight` semaphore
  released by the command buffer's completion handler. Every path that opens a
  frame must close it, including the one where there is no drawable: leak the
  permit three times and the app stops rendering with no error anywhere.
- **Nearest-corner ground sampling is fine until something moves.** It is exact
  at every corner and wrong everywhere between, so a moving unit holds its
  height for four elmos and then jumps the full height difference of the square.
  Placement and the sim must share ONE sampler, or a unit jumps on its first
  tick because the two disagreed about where the ground was.
- **Screenshots: capture the window by ID, not the screen.** With two displays
  and Spaces, `screencapture -x out.png` repeatedly grabbed bare wallpaper
  while the app was demonstrably presenting frames. Get the id from
  `CGWindowListCopyWindowInfo` and use `screencapture -o -l <id>`. Note also
  that AppleScript/`osascript` has no Accessibility permission here, so
  `System Events` window queries fail — use CoreGraphics directly.
