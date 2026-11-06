// SPIR-V rewrites made by the replay at pipeline time (vka_spvpatch.c). Plain C, no Vulkan
#pragma once
#include <stddef.h>
#include <stdint.h>
// The vertex module w (nw words) with the vec4 float Input variables at the locations of umask retyped as uvec4 and those of smask as ivec4, every load of them bit-cast back
// to float: the shader then sees the integer register bits Maxwell's fetch gives an integer format. A malloc'd copy (*out_nw words), or NULL with the reason in err
uint32_t *vka_spv_vin_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn);
// Subgroup operations and built-ins rewritten as subgroups of one invocation, for a stage the device runs none in (vka_spvpatch.c). A malloc'd copy (*out_nw words,
// *changed = 1), NULL with err empty when the module has none, NULL with the reason in err when it cannot be rewritten
uint32_t *vka_spv_subgroup_single(const uint32_t *w, size_t nw, size_t *out_nw, int *changed, char *err, size_t errn);
// A geometry shader's memory writes, counted (the return; -1 with err on failure) and, with out and a count above 0, the module with each of them made only by the run of
// the shader that first claims its primitive in the storage buffer at set 0 binding (zeroed for each draw, a word per primitive), malloc'd (*out_nw words): vka_spvpatch.c
int vka_spv_gs_claim(const uint32_t *w, size_t nw, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn);
// A pixel shader's float colour outputs at the locations of umask / smask as unsigned / signed integers of their bits (each store bit-cast), for integer attachments: malloc'd
// (*out_nw words), NULL with the reason in err (vka_spvpatch.c)
// Each read of the texture pool (set, binding) sets its ID's bit in a bitmap bound at (set, mark_binding): a malloc'd copy (*out_nw words, *nmarks reads marked), or NULL
// (err empty: nothing to mark; else the reason) (vka_spvpatch.c)
uint32_t *vka_spv_pool_mark(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, uint32_t mark_binding, size_t *out_nw, int *nmarks, char *err, size_t errn);
uint32_t *vka_spv_fout_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn);
// A pixel shader that may keep early depth / stencil tests once its pool reads are marked (asked of the module before the marks); its module with EarlyFragmentTests
int vka_spv_early_ok(const uint32_t *w, size_t nw, int alpha_test, uint32_t spec_alpha);
uint32_t *vka_spv_early_tests(const uint32_t *w, size_t nw, size_t *out_nw, char *err, size_t errn);
