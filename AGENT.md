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
- **Screenshots: capture the window by ID, not the screen.** With two displays
  and Spaces, `screencapture -x out.png` repeatedly grabbed bare wallpaper
  while the app was demonstrably presenting frames. Get the id from
  `CGWindowListCopyWindowInfo` and use `screencapture -o -l <id>`. Note also
  that AppleScript/`osascript` has no Accessibility permission here, so
  `System Events` window queries fail — use CoreGraphics directly.
