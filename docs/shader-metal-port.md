# Retail shaders → Metal port guide

How retail Forged Alliance renders — and what the Metal port must reproduce to
look like retail. Sources: 11 HLSL9 files, 12,343 lines, extracted from
`effects.scd` (staged at `build/re-shaders/`; SCDs are ZIPs — `unzip
effects.scd "effects/*.fx"`). Current Metal side: `src/render/shaders/*.hpp`
+ `src/render/*.mm`. Every section ends with **Port status** (done vs missing)
and **Quirks** (retail bugs to reproduce deliberately, flagged not fixed).

Conventions: `terrain.fx:694` = file:line in `build/re-shaders/`. `uniform
bool` VS/PS params = static branches (two pipeline variants each). D3D9 path is
the port target unless noted (D3D10 branches differ — flagged inline).

---

## 1. Terrain (`terrain.fx`, 1,567 lines)

### 1.1 Techniques (all single-P0 unless noted)

| Technique | Line | VS / PS | Role |
|---|---|---|---|
| `TTerrainDepth` | 817 | TerrainDepthVS / TerrainDepthPS | Depth prepass → VSM blur chain |
| `THorizontal/VerticalBlurDepthToVariance` | 828/840 | FixedFuncVS + blur PS | Separable 5×5 Gaussian (taps [1 4 6 4 1]/16, then (2,6,6,2)/16) → variance shadow map |
| `TTerrain` | 852 | TerrainVS(shadowed) / TerrainPS(shadows) | Main 4-stratum composite |
| `TTerrainXP` | 867 | same VS / TerrainAlbedoXP | 8-stratum composite, INLINE lighting (no CalculateLighting call) |
| `TTerrainGlow` | 884 | TerrainGlowVS / TerrainGlowPS | Stratum-1 alpha animated glow (`Time*0.125` sincos scroll, `outColor.a = glow*mask.y + 0.01`) |
| `TTerrainNormals` / `XP` | 901/913 | TerrainVS(false) / NormalsPS | Blended normals → RG channels (AlphaBlend write-RG-only) |
| `TTerrainBasis` / `BiCubic` | 926/939 | TerrainVS(false) / BasisPS | UtilityTextureA mask (`.xxwy` swizzle) → BA channels for low-fi path; BiCubic = Sigg-Hadwiger GPU-Gems-2 via 1D LUT |
| `TTerrainOverlay` | 951 | TerrainVS(false) / OverlayPS | Debug overlay (`tex2Dproj(Overlay)`), SrcAlpha |
| `TTerrainSkirt` | 964 | TerrainSkirtVS / SkirtPS | Map-edge skirt, flat `(0.1,0.1,0.1,0)`, cull-none |
| `TDecals` family (8) | 1231–1359 | DecalVS + per-mode PS | Lit / XP / GlowMask (`clip(a-0.90)`) / Glow-additive / WaterAlbedo / WaterMask / Normals(+Alpha). All LessEqual depth, no write, `Rasterizer_Bias_Decal` |
| `TSplats` / `LowFidelitySplat` | 1433/1446 | SplatsVS / SplatsPS(shadows,lowFi) | Per-vertex alpha (`mAlpha` TEXCOORD1); low-fi skips lighting |
| `LowFidelityTerrain` / `LowFidelityLighting` | 1503/1557 | two VS / two PS | Two-pass fallback: albedo lerp chain, then multiply (`Zero_SrcColor`); light PS reconstructs normal from basis (`.aag`, `g=sqrt(1-r²-b²)`) |

### 1.2 Vertex I/O (verified verbatim, `terrain.fx:459-534`)

- In: `position_t POSITION0` (uint4 D3D10 / float4 D3D9); `position.y *= HeightScale`.
- Out `VS_OUTPUT`: `mPos` (clip), `mTexWT = position.xzyw` (world, Y-up-in-Z),
  `mTexSS` = clip pos (screenspace normal fetch), `mShadow` (ShadowMatrix-projected,
  `x=(+x+w)*0.5, y=(-y+w)*0.5, z-=0.01` epsilon in VS), `mViewDirection =
  normalize(pos-CameraPos)`, `mTexDecal` (DecalMatrix UVs or glow scroll).
- `SampleScreen` (`:326`): `uv*ViewportScale + ViewportOffset*w`, `tex2Dproj`.

### 1.3 Stratum system (verified verbatim, `:694-723`)

Textures: Lower + Stratum0–7 + Upper albedo (10); Lower + Stratum0–7 normals (9);
`DECLARE_STRATUM` macro (`:50`): per-layer `float4 Tile` + texture + WRAP/LINEAR.
Masks: UtilityTextureA (strata 0–3) + UtilityTextureB (4–7, XP only).
Albedo passes expand `saturate(mask*2-1)`; normal passes use RAW masks
(Map.hpp already reproduces both readings). Blend: `albedo = lower;
lerp(albedo, stratumN, maskN)` strict 0→7 (chain of mixes, NOT weighted sum);
then `lerp(rgb, upper.rgb, upper.a)` (macrotexture on own alpha).
Normals: same chain over `(tex*2-1)`, normalize, output `(n*0.5+0.5, n.w)`.
**Gloss = `1-albedo.w`** (`:719`). UVs: `worldUV = mTexWT*TerrainScale`;
layer UV = `worldUV*LayerTile`; all `tex2Dproj`.

### 1.4 Lighting equation (verified verbatim, `:370-393`)

```hlsl
shadow = (inShadows && ShadowsEnabled==1) ? tex2D(ShadowSampler, shadow.xy/w).g : 1;
viewDirection = normalize(inViewPosition.xzy - CameraPosition);   // .xzy un-swizzle
SunDotNormal = dot(SunDirection, inNormal);
R = SunDirection - 2*SunDotNormal*inNormal;
specular = pow(saturate(dot(R, viewDirection)), 80) * SpecularColor.x * specAmount;
light = SunColor*saturate(SunDotNormal)*shadow + SunAmbience + specular;
light = LightingMultiplier*light + ShadowFillColor*(1-light);
color.rgb = light * inAlbedo;
color.rgb = ApplyWaterColor(waterDepth, color);   // lerp(rgb, WaterRamp(depth).rgb, WaterRamp(depth).a)
color.a = 0.01 + specular*SpecularColor.w;
```

XP variant (`:724`): same structure but shadow WITHOUT `/w` divide (quirk);
`specular = pow(...,80)*albedo.aaa*SpecularColor.a*SpecularColor.rgb`
(gloss = alpha³, ADDED to albedo BEFORE light multiply); alpha = 0.
Constants: specular power **80** everywhere; shadow epsilon 0.01 in VS;
`TerrainErrorScale 1.0005` sinks distant terrain (`:343`).

### 1.5 Uniforms & samplers (main path)

Matrices: View/Proj/Shadow/Decal/Tangent; NormalMapScale-Offset; ViewportScale-Offset;
TerrainScale; HeightScale. Light: SunDirection/SunColor/SunAmbience float3,
SpecularColor float4, LightingMultiplier, ShadowsEnabled, ShadowFillColor, Time.
Water: WaterRamp (+sampler CLAMP/LINEAR) + WaterElevation (Deep/Abyss DECLARED
NEVER USED — no elevation bands in this file). Textures: Utility A/B/C, NormalTexture
(screenspace, POINT), 19 stratum (WRAP/LINEAR), ShadowTexture ×3 samplers
(BORDER-white LINEAR / POINT / LINEAR), Decal set, BiCubicLookup (1D D3D9 vs 2D D3D10).
Unused declarations: HalfAngle, CameraDirection, SampleOffset, NoiseTexture (fog lives
in `frame.fx`), UpperNormal slots, decalDepthOffset.

### 1.6 Quirks (reproduce deliberately)

Y scaled TWICE in DepthVS (`:400-401`); XP shadow missing `/w`; decal-normals
double swizzle (`.ag→.xz`, net `(raw.a, raw.g, computedY)`); raw-vs-expanded mask
readings ~20 lines apart; FogVS clamp `#if 1` vs DecalsVS `#if 0`; water decals
ignore shadows (commented block `:1086-1091`); skirt is a constant despite
SkirtTexture; FixedFuncVS half-pixel offset wrong under D3D10 (comment admits);
XP gloss `albedo.aaa` vs non-XP `1-albedo.w`.

### 1.7 Port status vs `Map.hpp`/`MapRenderer.mm`

Done: 8-stratum albedo chain + expansion + upper macrotexture; raw-mask normals;
sun/ambient/multiplier/fill; PCF-ish shadow (replaces VSM with depth-compare);
fog-of-war mask; water plane (reimplementation, not WaterRamp tint).
Missing: specular term (pow80 + SpecularColor — documented drop); WaterRamp tint;
alpha `0.01+spec*w`; glow path + Time scroll; decals + splats (DecalMatrix plumbing);
skirt/overlay/basis targets; depth+VSM chain; low-fi two-pass; TerrainErrorScale sink;
VS shadow epsilon; UtilityTextureC.g water-depth texture.

---

## 2. Units (`mesh.fx`, 6,523 lines, 119 techniques; WINDOWS-1252/CRLF — `iconv` first)

### 2.1 Census (single-pass except noted)

- Core: `Unit_HighFidelity` (`:4706`, NormalMappedVS+PS maskAlbedo/glow/hiDef,
  stage DEPTH+REFLECTION+PREWATER+PREEFFECT, `PARAM_FRACTIONCOMPLETE`) / Med
  (drops PCF) / Low (VertexNormalVS+ColorMask, drops REFLECTION).
- Faction Hi/Med (+Lo ColorMask): Metal (aniso specular), Insect (insectTexture),
  Aeon (`AeonPhongCoeff*(pow3)`), Seraphim (falloffSampler view-ramp),
  AeonCZAR (scrolling energy trim into rgb AND alpha).
- Build anims: Aeon(+Puddle)/Cybran/Seraphim(no Hi)/UEF(+Cube) + overlays.
- Wreckage Med/Lo: sinusoidal crunch VS, **NO shadow receive** (`atten=1` hardcoded).
- Shields: PhaseShield 2-pass (opaque unit + 0.05-offset electricity shell);
  SeraphimPersonal, UEF (fraction-health), Cybran (α .17/.24), Aeon (α .5,
  REFLECTION), Seraphim (additive); ShieldFill/Impact/CybranImpact.
- Other: Depth ×4, Occlude/Silhouette, Cartographic ×6, Flat, VertexNormal ×3,
  NormalMappedAlpha ×12 (NoShadow/Undulating/Bloating/Blackened), Glow ×3,
  Clutter ×4, AlphaFade ×3, CommandFeedback/Rally/CmdFB, placement previews,
  Effect/Explosion/Cloud/Nuke ×10.

### 2.2 Skinning (rigid — no blending)

Inputs: POSITION/NORMAL/TANGENT/BINORMAL + uv + BLENDINDICES + model rows
(TEXCOORD1-4) + anim (`TEXCOORD5`: `.y`=bone base, `.w`=scroll) + material +
COLOR + colorLookup. Palettes `trans/rotPalette[80]`; quaternion→matrix
expansion (`:372-407`); world = palette × modelRows. **ALL VS use ONLY
`anim.y + boneIndex[0]`** — indices 1–3 never read. MSL: one bone fetch.
Scroll UV when `anim.w>0.5` (treads/energy). `CompatSwizzle(color)` is
engine-injected (undefined in-file) — no-op for MSL, verify team colors visually.

### 2.3 Unit lighting (verified: `NormalMappedPS :2170-2237`)

```hlsl
N = 2*tex(normals,uv2).gaa - 1; N.z = sqrt(1-x²-y²); normalize(mul(N, float3x3(BN,T,N)));  // .gaa swizzle
albedo = tex(albedo,uv); spec = tex(_SpecTeam,uv); env = texCUBE(env, reflect(-V,N));
albedo.rgb = lerp(vertex.color.rgb, albedo.rgb, 1-spec.a);   // A = TEAM MASK (1 keeps albedo)
phongAmount = saturate(dot(reflect(sunDirection,N),-V));
phongAdditive = PhongCoeff(0.6,0.80,0.90)*pow(phongAmount,2)*spec.g;   // exponent 2, broad
phongMult = 2*env*spec.r;
light = ComputeLight(dotLN, shadow);          // lightMultiplier*(sunDiff*sat(dot)*atten+sunAmb) + (1-l)*shadowFill
emissive = 2.0*spec.b;
color = albedo*(emissive+light+phongMult) + phongAdditive;
alpha = spec.b + 0.01;                        // units: effectively opaque, feeds bloom
```

Faction deltas: Aeon (0.6× dimmer, pow3, AeonPhongCoeff); Metal/Insect (aniso LUT on
`(NdotR,NdotL)`); Seraphim (falloff ramp); CZAR (energy trim); GlassAlpha
(true transparency `albedo.a+spec.b`); LoFi (`2*light²`, no shadow/env).
**No fresnel rim, no cloak/stealth/jammer terms anywhere** (zero grep hits) —
the `rim` term in Metal is a Recoil addition (correctly flagged); stealth visuals
are engine-side alpha state. Closest analogues: PhaseShield shells, build overlays,
`UnitPlacePS` ghost (α 0.2).

### 2.4 `_SpecTeam` convention (authoritative)

R = env reflectivity mult, G = additive-phong scale, B = emissive (×2.0),
**A = TEAM MASK** (inverted lerp). `Outline.hpp` SupCom branch already implements
exactly this. Recoil-family `tex1.a` is the other engine's convention — not retail's.

### 2.5 Quirks

`viewDirection = normalize(clip.xyz/w)*viewMatrix` (dimensionally wrong — keep);
Seraphim shadow forced 0 (`:2649`, full-shadow path); `LowFiUnitFalloffPS`
passes float3 as float attenuation (pick `atten=1.0`, note deviation);
Wreckage LoFi comma-operator drops height (`depth=material.x`); wreckage jitter
reads birth-clock not space; `CompatSwizzle` undefined-in-file; Blackened uses
flat `.1` gray weights.

### 2.6 Port status vs `Units.hpp`/`Outline.hpp`

Done: NormalMappedPS core, ComputeLight (shared with terrain), `.gaa` swizzle.
Missing: Aeon/Metal/Insect/Seraphim/Glass/Wreckage/LoFi paths (faction-pipeline
decision needed); build shaders; shield shells + impacts; wind/bloating vertex
anims; UV scroll; mirrored water-clip (Metal `clipBelowY` is the equivalent —
verify sign). Extras correctly flagged: fresnel rim, blast loop, Recoil branch.

---

## 3. Water / particles / sky / post

### 3.1 Water (`water2.fx`, 628 lines)

Techniques: `Water_LowFidelity` (2 SrcAlpha passes: flat + crest-alpha),
`Water_HighFidelity` (opaque + `NotEqual 0` alpha-test, LE/no-write, CW),
`TWaterLayAlphaMask` (returns `(0,0,0,MaskSampler.b)` — `.b` quirk),
`TShoreline` (returns `(0,0,0,0)` ALWAYS + `Equal 0` test, CCW quirk).
High-fi equation (16 ordered steps, verified): Utility.g depth → viewport-mapped
screenPos → Refraction bg + `mask=saturate(bg.a*255)` → 4 normal taps,
`waveCrest=saturate(sum.a-threshold)` → `N=2*sum.xyz-4`, `norm(N.xzy)` (swizzle
quirk), flatness lerp by `waterTexture.r` → sky-cube + refraction-with-bend
(`refractionScale*N.xz*OneOverW` — OneOverW-on-mapped-offset quirk) →
**Fresnel = 2D LUT** (`FresnelLookup(depth, saturate(dot(-view,N))).r`, NOT
Schlick) → glint `pow(dot(-R,SunDir),50)*SunColor`, planar-vs-sky by
`0.5*reflected.w`, **constant tint `clamp(depth,0.3,0.3)=0.3`** (depth ramp dead
by construction), depth×10 reflection gate, crest lerp by `(1-waterTexture.a)`.
Constants: `waterColor (0,0.7,1.5)`, fresnelBias/Power `0.1/1.5`, SunShininess 50,
`SunDirection norm(0.1,-0.967,0.253)`, `SunColor normalize(1.2,0.7,0.5)` (colour
quirk), `normalRepeatRate (0.0009,0.009,0.05,0.5)` + 4 scroll vectors,
reflection/refraction scales. `WaterRamp` DECLARED NEVER SAMPLED — no deep/abyss
bands in this file; do NOT invent a ramp.
Gaps vs Metal: 4-tap fetch→2 sines; bend-less framebuffer-fetch; mask/shore passes
absent (shore = `saturate(depth*0.25)` mix); analytic fresnel vs LUT quantization
(current choice — document); LowFidelity path absent.

### 3.2 Particles (`particle.fx`, 1331 lines, 68 techniques)

Matrix: 8 emitter families (TRamp/Animate/Align/…/Flat) × 6 blends
(MODULATEINVERSE `Zero,InvSrcColor` / MODULATE2XINVERSE `InvDestColor,InvSrcColor` /
ADD `(SrcAlpha,One)` / ALPHABLEND / PREMODALPHA / REFRACT) + TLight ×5 (screen-space
overlay, depth DISABLED — "light" in name only) + Beam ×10 (ADD is `(One,One)` —
differs from TRamp ADD) + PolyTrail ×5. All single-P0, Cull_None.
Vertex: dual streams (Corner −1..1 + Pos/Vel/Acc/birth/life/frames/drag);
`pos = Pos+Vt+½At²` EXCEPT drag branch (exponential `e^-kt` with literal
`2.71828183`, drops Pos base — quirk); Align NORMALIZES velocity (speed lost);
billboard via InverseViewMatrix rows; anim-strip UV + ramp-U (`alpha=t/life`);
dead particles emit degenerate verts. **NO soft-particle depth anywhere** —
fade lives in the RAMP texture only. Dead header uniforms (Position/Shape/Spread/
Speed/colors — never read); `DragEnabled` the only live switch.
Pixel: all UNLIT texture products (`tex0*tex1`); Trail hard-gates 0<t<1; BeamPS
returns UNINITIALIZED color outside {1,2} (do NOT reproduce — default 0);
refract = `0.005*(2*tex0.rg-1)` screen bend.
Sampler hazard: `ParticleSampler0` U=WRAP/V=CLAMP — MSL has one address mode per
sampler; use repeat + manual V saturate.
Gaps vs `Fx.hpp`: drag-exponential path absent (DragEnabled=1 content misbehaves);
double-fade risk (procedural × ramp); trail packing interpretive; REFRACT UV flip
unaudited; TLight/Beam/Trail/MODULATE2X pipelines missing (3 pipelines vs 68
techniques — minimum: additive-One-One beam/trail + depth-disabled light).

### 3.3 Sky (`sky.fx`, 353 lines)

Atmosphere (LUT-driven: azimuth mask × elevation ramp, NO sun disc, alpha 0);
Decal sun/moon sprites (albedo + Write_A glow for bloom chain); Cumulus lit
billboards (NdotL × dispersion ramps); Cirrus 4-layer multiplicative sheets
(any zero layer kills the pixel — quirk). Techniques all depth-off, CW.
Gaps vs Metal: analytic gradient vs LUT azimuth; NO Decal/Cumulus/Cirrus
counterparts (clouds = biggest sky gap); glow-alpha has no HDR-dest home.

### 3.4 Post (`frame.fx`, 744 lines)

Chain: `TCopyGlowingStuff` (dest-alpha extract, `MinimumGlow=0.02` reserves
water-signalling bottom) → H/V 7-tap separable blur (weights sum ~1.004, slight
gain quirk) → `TFrameAdd`/`TAdd` additive fold-back. Plus: Background/Strategic
dissolve (8× wrap, `Greater 0x7F`), Range/Vision/Boundary/Silhouette stencil
trio, Punchout, Basis (sqrt WITHOUT saturate — NaN risk, flag not port),
Opaque (returns RED — debug leftover), Black.
Gaps vs Metal: extract predicate (alpha-reservation vs luminance knee 0.8),
blur kernel (fixed 7-tap vs MPS gaussian), composite point (LDR-add vs linear-HDR
pre-tonemap) — glows WILL differ; dissolve/stencil overlays missing (stencil →
`MTLDepthStencilState` maps 1:1 when ported).

---

## 4. Aux (primbatcher / cartographic / vision / range / ui)

- **primbatcher.fx (471):** CPU-quad batcher — SkyBox, TRed/TYellow consts,
  AlphaBlend Linear/Point/NoDepth, TRange, TCommand(+Glow/Other), StrategicIcon
  (radius-0.5 recolor quirk), ResourceIcon 2-pass, LifeBar. One CompositeMatrix,
  `AlphaMultiplier` global; `CompatSwizzle` undefined-in-file (replicate, don't drop).
- **cartographic.fx (238):** strategic-zoom NPR terrain (hypsometric RGB +
  contour A → 3×3 edge filter → rotated world decals), NOT the minimap thumbnail.
  1D LUTs → Metal 2D strips at `(h.x, 0.5)` nearest. `elevMaximum/Minimum` unused.
- **vision.fx (203) / range.fx (79):** FoW/vision compositing; range rings are a
  STENCIL caster (elliptical disc volumes, low-7-bits gated on vision 0x80,
  composited by frame.fx Range/RangeBurn) — needs `MTLDepthStencilState`, not color.
- **ui.fx (206):** interface compositing (straightforward).
- **mesh.fx shield leftovers:** per-faction shells + cartographic unit variants
  (hypsometric tone bands `0.25/0.25/0.50/0.10` + elevation clip; ShieldPS has NO
  clip — quirk).

---

## 5. Global MSL port rules

- Semantics → `[[position]]` + stage_in; `COLOR0` → `[[color(0)]]`.
- `uniform bool/int` technique params → separate pipelines or `[[function_constant]]`.
- `sampler_state` → `constexpr sampler`; BORDER-white → clamp_to_border/edge + bounds check.
- `tex2Dproj(s,uv)` → `sample(s, uv.xy/uv.w)`; `tex1D` → 1D LUT or 2D strip at `(d,0.5)`.
- technique/pass → `MTLRenderPipelineState` (blend table: Disable/Write-masks/SrcAlpha-OneMinus/Additive/Zero-SrcColor-multiply) + depth (`LessEqual`+write flags) + `polygonOffset` for decal bias + cull (verify CW/CCW vs Metal winding on reflection pass).
- `clip(x)` / alpha-tests → `discard_fragment()`.
- Row-vector `mul(v,M)` → column-vector transpose; keep retail's sign conventions
  (`sunDirection` surface→sun; `.gaa`/`.xzy` swizzles; `1-albedo.w` gloss).
- vs_1_1/ps_2_0 vs D3D10: port DX9; D3D10-only branches become discards.
- Fidelity tiers (LOW/MED/HIGH) → quality-selected pipelines, not `#ifdef`.
- Reproduce-then-flag quirks list (§1.6, §2.5, water/ramp/sky/frame notes above);
  deviations (analytic fresnel, linear fog, no-dest-alpha glow) documented as accepted.

---

## 6. What to build first (look-alike priority)

1. Terrain specular + WaterRamp tint (cheapest big win; constants in hand).
2. Decal/splat pipeline (scorch, craters, range rings need it).
3. Aeon/Seraphim faction lighting + wreckage path (visible on every battlefield).
4. Shield shells (gameplay-critical readability).
5. Water 4-tap normals + fresnel LUT bake (pixel-exact option).
6. Clouds (Decal/Cumulus/Cirrus) + glow-chain alignment (accepted-deviation decision).
7. Cartographic strategic zoom + stencil overlays + build anims + wind anims.

Audit basis: 4 parallel auditors over staged `build/re-shaders/` + live
`src/render/**`; key equations re-verified verbatim by author (TerrainVS,
TerrainPS, CalculateLighting); outputs retained at
`agent://Sh(Terrain|Mesh|WaterFx|Aux)`.
