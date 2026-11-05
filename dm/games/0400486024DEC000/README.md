# The Legend of Zelda: Ocarina of Time (`0400486024DEC000`)

A Switch 2 game: version v131072 (the update `0400486024DEC800` over the base v65536), from a `.nup`. SDK 22.2.8 built for the Switch 2 (`sdk` names its spec `Ounce`), NVN2. Nintendo EPD's engine (sead / agl / gsys, Phive) with the RomFS layout of [Tears of the Kingdom](../0100F2C0115B6000/README.md) (`Banc`, `Chemical`, `Mals`, `RSDB`, `ActorSystem`, `Pack/*.pack.zs`). **In bring-up: not started on any device yet.**

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `subsdk0` is NintendoSDK movie (824 exports), as Tears of the Kingdom's; `main` imports from all three |
| `sys_resource 0x3000000` | `game.tsv` | the NPDM declares 48 MB of system resource: virtual address memory on, as [Mario Kart World](../0400C3F00006E000/README.md) (64 MB) |
| `mem_alias 0x100000000`, `mem_reserve 0x300000000`, `app_pool 0x240000000` | `game.tsv` | Mario Kart World's layout and the Switch 2's 9 GB pool, until this game's own is measured (`profile_guess.py` guessed the Switch 1 pool, 3.2 GB, which did not hold [Donkey Kong Bananza](../040074A01BF12000/README.md)'s heaps) |
| `render_cond on` | `game.tsv` | taken over from Donkey Kong Bananza and Tears of the Kingdom (agl's render buffer binding); not checked on this game |

## Builds

| Version | Display | Modules (build IDs) | Status |
|---|---|---|---|
| `v131072` | not known | main `e0f3ccfaf50065ae4ea01eb5aeb13b1a`, subsdk0 `b0110087a7b339ef74588d3bd0b4dd5d` (movie), sdk `1643a838814f9c10c696f930ed681e7f` (nnSdk 22.2.8) | registered (`build v131072`); rewritten for Android, one cut ([`patches.py`](patches.py)) |

Every module is ZBIC-compressed (NSO flag bit 7): reading them takes the `zbic` package ([ZBIC-compressed NSO segments](../../../docs/reference/packing.md#zbic-compressed-nso-segments)).

## Survey (2026-11-05, from the `.nup`)

- `sdk` makes the same 57 SVC calls as Donkey Kong Bananza's (`insn_census.py`: the same SVC numbers), so the SDK 22 kernel surface needs nothing new. The rewrite sites are of the kinds the rewriter already handles: `main` 50,390 `x18` uses and 1,264 `mrs tpidr_el0`, no `svc`; `sdk` 441 `mrs tpidrro_el0`, 45 `mrs cntpct_el0`. `main` is 15.8 M instructions (Donkey Kong Bananza's 7.2 M).
- `main` imports 1,183 functions; 659 of them are also imported by Donkey Kong Bananza's `main` (`nso_syms.py`). The rest are SDK functions the game's own `sdk` implements, over services the host may not answer yet: `nn::async` (41), `nn::audio` input streams, `nn::nfp` (amiibo), `nn::web`, `nn::ssl`, `nn::socket`, `nn::prepo`, `nn::hid` mouse.
- Shaders: `Shader/{Model,Foliage,TerrainGround,SceneMaterial}.Product.110.product.Nin_Ounce_NVN2.bfsha.zs` (the model archive is 43 MB compressed) and `Shader/ApplicationPackage.Nin_Ounce_NVN2.release.sarc.zs`.

## Shaders (2026-11-05)

`shader_split.py` finds 42,320 shaders (compute 4,100, mesh 9, pixel 22,356, task 3, vertex 15,852). The first Vulkan library build left 93 of them out; all build now, and a sample of 547 others (516 pixel, 18 vertex, 3 compute, 2 task and the 8 triangle meshes) translates to the same SPIR-V, byte for byte, as with the translator before these changes:

- **Resolved:** 74 pixel shaders query a 2D texture's size and read its third component (TXQ's depth): the SPIR-V extracted it from a two-component vector, which `spirv-val` refuses. A component the texture type does not have is now 1, as the texture header holds it (`CodeGen/Spirv/Instructions.cs` `GenerateTextureQuerySize`).
- **Resolved:** 7 shaders use `0x3a9`, the global compare-and-swap (`ATOMG.CAS`, Mesa NAK `sm70_encode.rs`): `InstEmitAmpereMem.cs` `AAtomgCas`, Rd the old value, 32-bit only.
- **Resolved:** 10 compute shaders (an RGB to YUV conversion) use `0x243`, `F2IP.U8.F32.NTZ` (base `0x043`; the name nvdisasm gives these encodings, in CuAssembler's sm_86 tables): two floats to unsigned bytes, a to byte 1 and b to byte 0 as F2FP's a high / b low; `PRMT 0x5410` then joins two such pairs, Cb before Cr (NV12 chroma). `InstEmitAmpereAlu.cs` `AF2ip`.
- **Resolved:** a task program reads back its own task memory (`0x923` with the mode `0xb800` its stores use): `InstEmitAmpereMesh.cs`.
- **Resolved:** `Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_common.sharcb#BAHS@0x357f0` was not a shader: the BAHS table scan paired a compute control section with a vertex + pixel program's code, and the translator stopped on its SPH word as "Ampere instruction 0x481". `shader_split.py` now leaves such a match out with a red line. Mario Kart World's compute `078997bf631992c5` fails the same way and may be the same kind of match (not checked).
- **Resolved:** mesh program `2582bd7ea6490d33` (`Shader/ApplicationPackage.Nin_Ounce_NVN2.release.sarc.zs/AglShader.sharcb`) draws points: its control section has 1 at +0x5f where the 8 triangle meshes have 3 (and 32 vertices / 32 primitives at most), it writes one index byte a primitive at 4 + t, and the point size besides position (SPH output map byte 0x35 bit 3). Its stores put the point size in plane 0, before x y z w (a depth-scaled scalar there, the four matrix rows after it), the order of the attribute addresses (0x6c, then 0x70). `mesh_facts` gives the point size plane code 132 and marks the topology (`-4, 1`; nothing for triangles, so the other layouts are unchanged); the translator declares `PointSize`, `PrimitivePointIndicesEXT` and `OutputPoints` (`MeshShaderInfo.cs`, `SpirvMeshEntry.cs`).

- **Taken from the host's saves:** 11 compute shaders the game binds from the intro on are in no module and in nothing the split reads; with `DM_NVN_SHDUMP` the Vulkan replay saved them (the intro and the title menu stayed black without them), and the split takes them from `DM_NVN_SHDUMP_DIR` (42,331 shaders, compute 4,111). Where in the RomFS they live is not found yet. They are agl's NDVE network for movies (below), which the game no longer runs since its cut.

## First start on Android (2026-11-05, AYN Thor, Turnip on Adreno 740)

- **Resolved:** `auddmg:u` (the audio device manager the SDK 22 audio client opens at startup): served by the host, its command 0 returning a proxy session ([`hle_sf.c`](../../host/hle_sf.c)).
- **Resolved:** the gRPC event engine's wakeup descriptor (`nn::socket::EventFd`): host eventfds ([`hle_net.c`](../../host/hle_net.c)).
- **Resolved:** SIGILL at `autiasp` in `DispatchFiberContext` (`sdk+0x16d40c`): Android gives each thread its own pointer authentication keys ([Pointer authentication keys differ per thread on Android](../../../docs/reference/threads.md#pointer-authentication-keys-differ-per-thread-on-android)).
- **Resolved:** the abort at `subsdk0+0xf9bc` when the intro movie starts: its AV1 decoder ([AV1 on Android through MediaCodec](../../../docs/reference/movies.md#av1-on-android-through-mediacodec)).
- The game reaches its title menu at 60 frames a second with its sound; the picture was black (the 11 compute shaders above).
- **Cut:** the 3D scene stayed black, its sound playing. agl upscales the scene into the output target (1536x864 → 2048x1152 docked, into 1920x1080 handheld) with NDSR, three HMMA compute networks (`f8965bffbdf51d78`, `a335c1db6270c355`, `fa3033d05c0a41d6`, from `Lib/agl/…/agl_ndsr_vr*.pck`), bound from the upscale step `main+0x25aa60` (call at `main+0x25ac28`, found with `DM_NVN_PROGTRACE`). On Turnip they write NaN into every texel of the target (`DM_VK_NANSCAN` saw it only at the next render pass, the screen fade, since the target is a compute pass's storage image; `DM_VK_KEYDUMP` showed that draw's buffers and vertices sane) and cost about 87 ms of GPU a frame. The render configuration's "use NDSR" byte (+0x9c0) is set by its constructor and again by the game's graphics parameters; the cut in [`patches.py`](patches.py) makes its four reads load 0 (`main+0x2503a8`, `+0x25aad0`, `+0x534e18`, `+0x654f04`), and the scene renders (Kokiri Forest's stream, 2026-11-05). Why the networks give NaN on Vulkan (the HMMA lowering, `hmma_lds`) is open.
- **Resolved:** a crash at `AddressInfo::GetReference` (`sdk+0x249874`, null + 0x20) after about 200 s on the title menu, the same in handheld mode: the host locked SDK 20's update lock (+0x2d0) where SDK 22.2 has it at +0x2d8 ([The renderer's update lock](../../../docs/reference/audio.md#the-renderers-update-lock-update_lock)).
- **Resolved:** the abort (`Result 0xe15`, `prepo:u`) when the intro movie ends: `prepo::PlayReport::Send(Uid)` ([An async context returned through x8](../../../docs/reference/services.md#an-async-context-returned-through-x8-prepoplayreportsend)).
- **Cut:** the intro movie stayed black with its sound, at about 1 frame a second. agl's movie renderer (vtable slot +0x18, `main+0x3b63360`) runs NDVE: the 11 compute programs above (HMMA, `DM_NVN_PROGTRACE` traced every bind to `main+0x20e57e0`, called from `main+0x3b634b0`) turn the 1920x1080 Y and UV planes into a 2560x1440 `NDVE_Output`, then draw it. On Turnip they take about 533 ms of GPU a frame, and `d15370a382630d4e` runs the driver out of memory while compiling (skipped with `DM_VK_CS_SKIP`). The renderer has its own plain path, a YUV → RGB textured draw (`main+0x3b62450` → `main+0x3539360`), taken when the byte at `main+0x4e95f61` is set or there is no NDVE object; the cut in [`patches.py`](patches.py) makes the byte test always take it (`tbz` → `nop` at `main+0x3b63368`), on every target.

- **Resolved (partly):** the 3D scene's speed on the AYN Thor. Raw global memory first went through a 12-step search of the pool table inlined into every access: a pass of 184 ms of GPU a frame. A cache of the last pool brought the frame to about 74 ms (40 ms of GPU). The page table that replaced the search on Android ([Global memory at an address loaded from memory](../../../docs/reference/shaders.md#global-memory-at-an-address-loaded-from-memory)) brought it to about 65 ms (36.5 ms of GPU).
  - What still costs: vertex shader `4671e5e647fc5455` (2 draws, 87,576 indices) reads memory through a pointer 253 times a vertex. Its pass takes about 20 ms. Turnip's statistics (`DM_VK_PSTATS`) put the shader at 18,008 instructions with the page table, against 50,769 with the search.
  - Measured with `DM_VK_GPUPROF`, `DM_VK_DRAW_SKIP` and `DM_VK_XP_SCISSOR`: the pass is bound by its vertices, not its pixels.
  - Keeping the last page looked up per invocation made it no faster (22 ms). The memory loads themselves are the likely cost; reading through bound storage buffers instead is not tried yet.

## Open

- The picture on Android with the movie and NDSR cuts: the 3D scene renders at about 15 fps (see above), and the mesh draws Turnip cannot run are open (`DrawMeshTasksIndirect`, no `VK_EXT_mesh_shader`).
- The title menu's text is missing; its panels draw. Findings so far:
  - The text box under the logo is pixel shader `b998f78a92860333`. Its four RGTC1 textures are only the band's decoration (`DM_VK_TEXDUMP`).
  - No draw of the title screen holds glyphs, and the host skips none: the game emits no text.
  - The scalable fonts (`Font2/Scalable/*.bfotf`) and the English messages (`Mals/USen.Product.110.sarc`) load (`DM_FS`). `tex_flush_upload` and `tex_dirty_upload` change nothing.
  - The texture text thread (`eui::FontMgr` + 0x118) only serves explicitly registered boxes (the button guide's icons), which the title does not open.
  - The title's text boxes are calculated each frame (`main+0x150b670`), build their glyphs on the frame (`main+0xe4e0e0`) and have their font (`+0xe8` set): what stops is further on. Either the font's glyph texture pages (its vtable slots `+0xe0`, `+0xa8`, `+0xc8`) or the text box's draw.
  - The observation gates in [`patches.py`](patches.py) are temporary. Where the byte at `main+0x4e95f61` is set (a setting, or a debug switch) is not looked at.
- The memory layout is not measured (see above).
- The translator changes move the chain version for every game: they need `scripts/regress.py check` on a Mac before they can go upstream.
