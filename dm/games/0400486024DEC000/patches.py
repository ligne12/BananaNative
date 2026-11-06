# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a
# module's NSO build ID; CUTS as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
CUTS = {
  # v131072 main: movies take agl's plain YUV → RGB draw, not the NDVE network. The movie renderer's draw (vtable slot +0x18, main+0x3b63360) picks one of two paths:
  # with the byte at main+0x4e95f61 set, or with no NDVE object (+0x2e8), it draws the Y and UV planes straight to the target (main+0x3b62450 → main+0x3539360, a textured
  # draw that scales); otherwise it runs NDVE (main+0x3b6337c → main+0x20e57e0): 11 HMMA compute programs that convert and upscale the 1920x1080 frame into a 2560x1440
  # NDVE_Output, then draw that. On the AYN Thor (Turnip) the network takes about 533 ms of GPU per frame and one of its programs (d15370a382630d4e) runs the driver out of
  # memory while compiling: the intro stays black with its sound playing. The cut makes the byte test always take the plain path (tbz → nop).
  'e0f3ccfaf50065ae4ea01eb5aeb13b1a': [
    (0x3b63368, '68000036', '1f2003d5', 'MovieRenderer draw: tbz w8,#0 (NDVE when the byte at +0x4e95f61 is clear) → nop: always the plain YUV draw, not the NDVE network'),
    # The 3D scene: agl's render configuration has a "use NDSR" byte at +0x9c0 (its constructor sets it, main+0xd16590; the game's graphics parameters set it again later:
    # clearing the constructor's and the loader's stores of 1 left it on). The renderer reads it at four places, each then also asking a per-view enable mask
    # (main+0x2503a8, main+0x25aad0 in the upscale step main+0x25aa60, main+0x534e18, main+0x654f04). NDSR upscales the scene into the output target (1536x864 → 2048x1152
    # docked, into 1920x1080 handheld) with three HMMA networks (f8965bffbdf51d78, a335c1db6270c355, fa3033d05c0a41d6, from agl_ndsr_vr*.pck, called at main+0x25ac28);
    # on Turnip they write NaN into every texel (the picture stays black) and cost about 87 ms of GPU a frame. Each read loads 0 instead: NDSR off everywhere at once
    (0x2503a8, '29016739', '09008052', 'renderer: ldrb w9, [x9, #0x9c0] (use NDSR) → mov w9, #0'),
    (0x25aad0, '8c016739', '0c008052', 'upscale step: ldrb w12, [x12, #0x9c0] (use NDSR) → mov w12, #0'),
    (0x534e18, '08016739', '08008052', 'renderer: ldrb w8, [x8, #0x9c0] (use NDSR) → mov w8, #0'),
    (0x654f04, '4a016739', '0a008052', 'renderer: ldrb w10, [x10, #0x9c0] (use NDSR) → mov w10, #0'),
  ],
}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': "agl movie renderer's NDVE path skipped", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "the movie renderer's vtable and its byte test are agl's (Nintendo EPD's engine), so a fingerprint of the draw function would find it in the engine's other games "
            'that ship NDVE; it can go once the network runs fast enough on the target GPUs',
     'checked': 'main v131072 disassembled: the byte test, both paths and the vtable slot (main+0x4c65b38), 2026-11-05'},
    {'what': "agl's NDSR scene upscaler turned off", 'now': 'build', 'goal': 'engine', 'purpose': 'rendering',
     'why': "the render configuration's NDSR byte is agl's (Nintendo EPD's engine), so a fingerprint of its constructor would find it in the engine's other games; it can go "
            'once the HMMA networks run right and fast enough on the target GPUs',
     'checked': 'main v131072 disassembled and decompiled: the four reads and the upscale step (main+0x25aa60), the NDSR binds traced to main+0x25ac28, 2026-11-05'},
]
