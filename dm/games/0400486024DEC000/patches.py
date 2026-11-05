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

# Temporary observation gates (OBS_TRACE = 27, the trace point in bits 8 up; hle.c obs_trace logs the registers): why the title menu draws its text boxes' panels but no text.
# The text boxes' texture thread (UpdateTextBoxTextureThread, eui::FontMgr + 0x118) never received a message; a pane's update (vtable method main+0x2d1380, shared by the pane classes)
# sends one only when the pane has a request object (+0x278) whose first word is set. Remove once the cause is known
OBS = {
  'e0f3ccfaf50065ae4ea01eb5aeb13b1a': [
    (0x2d1380, 27 | 1 << 8, 'd503237f', 'trace 1: pane update entry (x0 the pane)'),
    (0x2d2808, 27 | 2 << 8, 'b9400109', 'trace 2: pane update, the pane has a text request object (x8; [x8] its pending word)'),
    (0x2d2818, 27 | 3 << 8, 'f9401268', 'trace 3: pane update sends the request to UpdateTextBoxTextureThread'),
    (0xa7f3c0, 27 | 4 << 8, 'd503237f', 'trace 4: eui::FontMgr font by name (x1 the name)'),
    (0xa7e9c0, 27 | 5 << 8, 'd503237f', 'trace 5: eui::FontMgr font resource load (x1 its description)'),
    # The only maker of the request object (+0x278) is main+0x439ac0 (x0 a text box): it returns at once when [[box+0x18]+0xb0] (its layout's owner) is null
    (0x439ac0, 27 | 6 << 8, 'd503237f', 'trace 6: a text box registers with the texture thread (x0 the text box)'),
    (0x439aec, 27 | 7 << 8, 'f9401516', 'trace 7: its layout has an owner ([+0xb0] set, x8)'),
    (0x439b94, 27 | 8 << 8, 'f9013ed5', 'trace 8: the text request (+0x278) is made'),
    (0x2d278c, 27 | 9 << 8, 'f9413e68', 'trace 9: pane update reaches the request test before the send'),
    # The request lands on the layout's owner (L+0x28), which the update loop main+0x5391e0 never updates (it updates two other panes): why
    (0x772034, 27 | 10 << 8, 'f9416508', 'trace 10: a layout root is bound to its owner (x0 the owner, x8 its vtable, x21 the root)'),
    (0x4b6110, 27 | 11 << 8, 'f9401008', 'trace 11: a pane object is activated (x0 the object, w1 the level, x8 its old +0x1a5)'),
    (0x5392e0, 27 | 12 << 8, '79436008', 'trace 12: the update loop reaches an object whose state is set (x0 the object, x8 its id, x22 the position)'),
    (0x5392ec, 27 | 13 << 8, 'f9404508', 'trace 13: the update loop calls the object update (x0 the object, x8 its vtable)'),
    # The owner is never activated (trace 11): panes are activated by a UI flow graph's node entry (main+0x74d7b0) or by an open-by-name request (main+0xc66420)
    (0x773a00, 27 | 14 << 8, 'd503237f', 'trace 14: a pane makes its root layout holder (x0 the pane, x1 its heap, lr the caller)'),
    (0x74d7b0, 27 | 15 << 8, 'd503237f', 'trace 15: a flow node is entered (x0 the node, lr the caller)'),
    (0xc66478, 27 | 16 << 8, 'f868d920', 'trace 16: an open-by-name request resolved its pane (x0 the id found, x19 the request)'),
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
