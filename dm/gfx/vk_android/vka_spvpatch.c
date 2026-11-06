// SPIR-V rewrites the replay makes at pipeline time (plain C, no Vulkan: unit-tested on the Mac by dm/host/tests/t_android_spvpatch.c).
//
// vka_spv_vin_retype: vertex inputs fed integer formats. The translator declares every vertex input a vec4 of floats (the library's vin_types say float for all of
// Donkey Kong Bananza's 65,846 inputs) and a shader that uses one as integer bits bit-casts it. On Maxwell an attribute of an integer format (UINT8 … SINT32) puts the
// integer, widened by its own sign, into the 32-bit register the shader reads, so those bits are what the bit-cast must see (bone indices, packed data). Vulkan gives a float
// input only a float, normalized or scaled format, and a USCALED stand-in hands the shader the bits of 5.0 instead of 5. So for a pipeline whose integer attributes feed float
// inputs, the vertex module is rewritten: those inputs become uvec4 / ivec4 (Input variables of the integer vector type), and each load of one, or of a component through an
// access chain, is followed by an OpBitcast back to the float type the body expects. The bits are then Maxwell's, and the attribute keeps its own integer format.
// nvn_mtl.m reaches the same bits by converting such attributes on the CPU into float32 bits (vconv_int); the Apple chain's dmsh_vin is the reverse case (integer inputs fed float formats).
#include "vka_spvpatch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { OpExtInst = 12, OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54,
       OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpBitcast = 124,
       OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633 };
enum { DecLocation = 30, SCInput = 1 };

typedef struct { uint32_t *w; size_t n, cap; } vec_t;
static int put(vec_t *v, uint32_t x) {
    if (v->n == v->cap) { size_t c = v->cap ? 2 * v->cap : 4096; uint32_t *p = realloc(v->w, c * 4); if (!p) return 0; v->w = p; v->cap = c; }
    v->w[v->n++] = x; return 1;
}
static int put_n(vec_t *v, const uint32_t *x, size_t n) { for (size_t i = 0; i < n; i++) if (!put(v, x[i])) return 0; return 1; }

// vka_spv_pool_mark: which texture IDs a shader reads through the pool (a handle loaded from memory: the bindless table, set `set` binding `binding`) is known only on the GPU,
// and with DM_VK_TEX_LAZY an ID gets its image only at a use the host sees (vka_tex.c "Texture IDs read through the pool"). So after each OpAccessChain into a pool variable
// the module calls an added function with the index: word = id >> 5, bit = 1 << (id & 31) of a bitmap of uint words bound at (set, mark_binding); the word is read first and
// only a clear bit is set (OpAtomicOr), so nearly every read costs a load (the Mac host's dm_pool_mark, nxnative "Shaders report which textures they read through a pool").
// An index of a type other than a 32-bit integer, or whose definition is not one of the common integer instructions below, is left unmarked. SPIR-V 1.3 and later: the
// bitmap is a StorageBuffer variable, which from 1.4 on every entry point lists in its interface (added to each OpEntryPoint)
uint32_t *vka_spv_pool_mark(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, uint32_t mark_binding, size_t *out_nw, int *nmarks, char *err, size_t errn) {
    enum { OpTypeVoid = 19, OpTypeBool = 20, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypeFunction = 33, OpConstant = 43, OpFunctionParameter = 55, OpFunctionCall = 57,
           OpLabel = 248, OpSelectionMerge = 247, OpBranch = 249, OpBranchConditional = 250, OpReturn = 253, OpFunctionEnd = 56, OpShiftRightLogical = 194, OpShiftLeftLogical = 196,
           OpBitwiseAnd = 199, OpIEqual = 170, OpAtomicOr = 242, OpCopyObject = 83, OpCompositeExtract = 81, OpSelect = 169, OpIAdd = 128, OpISub = 130, OpIMul = 132,
           OpUConvert = 113, OpSConvert = 114, OpBitwiseOr = 197, OpBitFieldUExtract = 202, OpPhi = 245, OpExtInstOp = 12, OpUndef = 1,
           DecBinding = 33, DecDescriptorSet = 34, DecBlock = 2, DecOffset = 35, DecArrayStride = 6, SCStorageBuffer = 12 };
    *out_nw = 0; *nmarks = 0; if (errn) err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    if (w[1] < 0x00010300) { snprintf(err, errn, "SPIR-V version %#x (before 1.3 no StorageBuffer class)", w[1]); return NULL; }
    int iface = w[1] >= 0x00010400;   // (from 1.4 an entry point's interface lists every global variable it uses)
    uint32_t bound = w[3];
    uint32_t *dset = calloc(bound, 4), *dbind = calloc(bound, 4), *type_of = calloc(bound, 4), *def_at = calloc(bound, 4);   // per id; def_at: the defining instruction's word + 1
    uint8_t *isint = calloc(bound, 1);   // 1: an OpTypeInt 32 unsigned, 2: signed
    size_t *site = NULL; int nsite = 0, csite = 0;
    if (!dset || !dbind || !type_of || !def_at || !isint) { snprintf(err, errn, "no memory"); goto fail; }
    for (uint32_t i = 0; i < bound; i++) dset[i] = dbind[i] = ~0u;
    uint32_t t_void = 0, t_bool = 0, t_uint = 0, t_fn = 0, c[4] = {0};   // c: uint constants 0, 1, 5, 31
    static const uint32_t cval[4] = {0, 1, 5, 31};
    size_t first_dec = 0, first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, ln = w[i] >> 16;
        if (!ln || i + ln > nw) { snprintf(err, errn, "bad instruction at word %zu", i); goto fail; }
        if (op == OpDecorate && ln >= 4 && w[i + 1] < bound) { if (!first_dec) first_dec = i; if (w[i + 2] == DecDescriptorSet) dset[w[i + 1]] = w[i + 3]; if (w[i + 2] == DecBinding) dbind[w[i + 1]] = w[i + 3]; }
        else if ((op == OpMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString) && !first_dec) first_dec = i;
        else if (op == OpTypeVoid && ln == 2) t_void = w[i + 1];
        else if (op == OpTypeBool && ln == 2) t_bool = w[i + 1];
        else if (op == OpTypeInt && ln == 4 && w[i + 2] == 32 && w[i + 1] < bound) { isint[w[i + 1]] = w[i + 3] ? 2 : 1; if (!w[i + 3]) t_uint = w[i + 1]; }
        else if (op == OpTypeFunction && ln == 4 && t_void && t_uint && w[i + 2] == t_void && w[i + 3] == t_uint) t_fn = w[i + 1];
        else if (op == OpConstant && ln == 4 && t_uint && w[i + 1] == t_uint) { for (int k = 0; k < 4; k++) if (!c[k] && w[i + 3] == cval[k]) c[k] = w[i + 2]; }
        else if (op == OpFunction && !first_fn) first_fn = i;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && ln >= 5 && w[i + 3] < bound && dset[w[i + 3]] == set && dbind[w[i + 3]] == binding) {
            if (nsite == csite) { csite = csite ? 2 * csite : 16; size_t *p = realloc(site, (size_t)csite * sizeof *p); if (!p) { snprintf(err, errn, "no memory"); goto fail; } site = p; }
            site[nsite++] = i;
        }
        switch (op) {   // result type and id at words 1 and 2: the integer instructions an index is made with
        case OpLoad: case OpBitcast: case OpCopyObject: case OpCompositeExtract: case OpSelect: case OpIAdd: case OpISub: case OpIMul: case OpUConvert: case OpSConvert:
        case OpBitwiseAnd: case OpBitwiseOr: case OpShiftRightLogical: case OpShiftLeftLogical: case OpBitFieldUExtract: case OpPhi: case OpExtInstOp: case OpConstant: case OpUndef:
            if (ln >= 3 && w[i + 2] < bound) { type_of[w[i + 2]] = w[i + 1]; def_at[w[i + 2]] = (uint32_t)i + 1; }
            break;
        }
        i += ln;
    }
    if (!nsite) goto fail;   // (nothing to mark: err stays empty)
    if (!first_dec || !first_fn) { snprintf(err, errn, "no decoration or no function"); goto fail; }
    for (size_t i = 5; i < first_fn;) { uint32_t op = w[i] & 0xffff, ln = w[i] >> 16; if (op == OpTypeFunction && ln == 4 && t_void && t_uint && w[i + 2] == t_void && w[i + 3] == t_uint) t_fn = w[i + 1]; i += ln; }   // (one declared before uint)
    vec_t o = {0}; uint32_t nid = bound;
    uint32_t n_void = t_void ? t_void : nid++, n_bool = t_bool ? t_bool : nid++, n_uint = t_uint ? t_uint : nid++, n_fn = t_fn ? t_fn : nid++, n_c[4];
    for (int k = 0; k < 4; k++) n_c[k] = c[k] ? c[k] : nid++;
    uint32_t p_u = nid++, rt = nid++, st = nid++, p_s = nid++, var = nid++, fn = nid++, par = nid++, l0 = nid++, l1 = nid++, l2 = nid++,
             wd = nid++, b = nid++, bit = nid++, pw = nid++, v = nid++, has = nid++, z = nid++, at = nid++;
    if (!put_n(&o, w, 5)) goto nomem;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, ln = w[i] >> 16;
        if (i == first_dec) {   // the bitmap's decorations
            uint32_t d[] = {4u << 16 | OpDecorate, var, DecDescriptorSet, set, 4u << 16 | OpDecorate, var, DecBinding, mark_binding, 3u << 16 | OpDecorate, st, DecBlock,
                            5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0, 4u << 16 | OpDecorate, rt, DecArrayStride, 4};
            if (!put_n(&o, d, sizeof d / 4)) goto nomem;
        }
        if (i == first_fn) {   // its types, constants and variable, at the end of the global declarations
            if (!t_void && (!put(&o, 2u << 16 | OpTypeVoid) || !put(&o, n_void))) goto nomem;
            if (!t_bool && (!put(&o, 2u << 16 | OpTypeBool) || !put(&o, n_bool))) goto nomem;
            if (!t_uint) { uint32_t d[] = {4u << 16 | OpTypeInt, n_uint, 32, 0}; if (!put_n(&o, d, 4)) goto nomem; }
            if (!t_fn) { uint32_t d[] = {4u << 16 | OpTypeFunction, n_fn, n_void, n_uint}; if (!put_n(&o, d, 4)) goto nomem; }
            for (int k = 0; k < 4; k++) if (!c[k]) { uint32_t d[] = {4u << 16 | OpConstant, n_uint, n_c[k], cval[k]}; if (!put_n(&o, d, 4)) goto nomem; }
            uint32_t d[] = {4u << 16 | OpTypePointer, p_u, SCStorageBuffer, n_uint, 3u << 16 | OpTypeRuntimeArray, rt, n_uint, 3u << 16 | OpTypeStruct, st, rt,
                            4u << 16 | OpTypePointer, p_s, SCStorageBuffer, st, 4u << 16 | OpVariable, p_s, var, SCStorageBuffer};
            if (!put_n(&o, d, sizeof d / 4)) goto nomem;
        }
        if (op == OpEntryPoint && iface) {   // the bitmap at the end of its interface
            if (!put(&o, (ln + 1) << 16 | OpEntryPoint) || !put_n(&o, w + i + 1, ln - 1) || !put(&o, var)) goto nomem;
            i += ln; continue;
        }
        if (!put_n(&o, w + i, ln)) goto nomem;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && ln >= 5 && w[i + 3] < bound && dset[w[i + 3]] == set && dbind[w[i + 3]] == binding) {
            uint32_t idx = w[i + 4], ty = idx < bound ? type_of[idx] : 0;
            if (!ty || ty >= bound || !isint[ty]) { i += ln; continue; }   // (not a 32-bit integer the walk knows: unmarked)
            if (isint[ty] == 2) {   // a signed index: the uint it was cast from, else a cast
                uint32_t d = def_at[idx];
                if (d && (w[d - 1] & 0xffff) == OpBitcast && (w[d - 1] >> 16) == 4 && w[d + 2] < bound && type_of[w[d + 2]] == n_uint) idx = w[d + 2];
                else { uint32_t u = nid++, x[] = {4u << 16 | OpBitcast, n_uint, u, idx}; if (!put_n(&o, x, 4)) goto nomem; idx = u; }
            }
            uint32_t call[] = {5u << 16 | OpFunctionCall, n_void, nid++, fn, idx};
            if (!put_n(&o, call, 5)) goto nomem;
            (*nmarks)++;
        }
        i += ln;
    }
    {   // the mark: void fn(uint id)
        uint32_t d[] = {5u << 16 | OpFunction, n_void, fn, 0, n_fn, 3u << 16 | OpFunctionParameter, n_uint, par, 2u << 16 | OpLabel, l0,
                        5u << 16 | OpShiftRightLogical, n_uint, wd, par, n_c[2], 5u << 16 | OpBitwiseAnd, n_uint, b, par, n_c[3], 5u << 16 | OpShiftLeftLogical, n_uint, bit, n_c[1], b,
                        6u << 16 | OpAccessChain, p_u, pw, var, n_c[0], wd, 4u << 16 | OpLoad, n_uint, v, pw, 5u << 16 | OpBitwiseAnd, n_uint, has, v, bit,
                        5u << 16 | OpIEqual, n_bool, z, has, n_c[0], 3u << 16 | OpSelectionMerge, l2, 0, 4u << 16 | OpBranchConditional, z, l1, l2,
                        2u << 16 | OpLabel, l1, 7u << 16 | OpAtomicOr, n_uint, at, pw, n_c[1], n_c[0], bit, 2u << 16 | OpBranch, l2,   // (scope 1 Device, semantics 0 Relaxed)
                        2u << 16 | OpLabel, l2, 1u << 16 | OpReturn, 1u << 16 | OpFunctionEnd};
        if (!put_n(&o, d, sizeof d / 4)) goto nomem;
    }
    if (!*nmarks) { free(o.w); goto fail; }   // (every index of an unknown type: nothing marked, err empty)
    o.w[3] = nid; *out_nw = o.n;
    free(dset); free(dbind); free(type_of); free(def_at); free(isint); free(site);
    return o.w;
nomem:
    free(o.w); snprintf(err, errn, "no memory");
fail:
    free(dset); free(dbind); free(type_of); free(def_at); free(isint); free(site);
    return NULL;
}

uint32_t *vka_spv_vin_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL; vec_t o = {0};   // cls[id]: 0 untouched, 1 a retyped input or a chain into one, as uint, 2 as int (bit 4: the id is a pointer to a vec4)
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!(cls = calloc(bound, 4))) { snprintf(err, errn, "out of memory"); return NULL; }
    if ((umask | smask) == 0 || (umask & smask)) FAIL("no input, or an input both uint and int");
    // Pass 1: locations, the types, the Input variables
    uint32_t *loc = calloc(bound, 4); if (!loc) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, t_u32 = 0, t_i32 = 0, t_v4f = 0, t_v4u = 0, t_v4i = 0, p_in_v4f = 0;
    uint32_t p_in[2][2] = {{0}};   // [uint/int][scalar/vec4] Input pointers that exist
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) { free(loc); FAIL("bad instruction at word %zu", i); }
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3];
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32) t_f32 = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpTypeVector && len >= 4 && w[i + 3] == 4) {
            if (w[i + 2] == t_f32 && t_f32) t_v4f = w[i + 1]; else if (w[i + 2] == t_u32 && t_u32) t_v4u = w[i + 1]; else if (w[i + 2] == t_i32 && t_i32) t_v4i = w[i + 1]; }
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCInput) {
            uint32_t t = w[i + 3];
            if (t == t_v4f && t) p_in_v4f = w[i + 1];
            for (int c = 0; c < 2; c++) { if (t == (c ? t_i32 : t_u32) && t) p_in[c][0] = w[i + 1]; if (t == (c ? t_v4i : t_v4u) && t) p_in[c][1] = w[i + 1]; }
        }
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!t_f32 || !t_v4f || !p_in_v4f) { free(loc); FAIL("no vec4 float input type"); }
    // New ids for the types the module lacks
    uint32_t nb = bound;
    int need_c[2] = {(umask != 0), (smask != 0)};
    uint32_t t_s[2] = {t_u32, t_i32}, t_v[2] = {t_v4u, t_v4i};
    uint32_t mk_s[2] = {0}, mk_v[2] = {0}, mk_p[2][2] = {{0}};
    for (int c = 0; c < 2; c++) if (need_c[c]) {
        if (!t_s[c]) t_s[c] = mk_s[c] = nb++;
        if (!t_v[c]) t_v[c] = mk_v[c] = nb++;
        if (!p_in[c][0]) p_in[c][0] = mk_p[c][0] = nb++;
        if (!p_in[c][1]) p_in[c][1] = mk_p[c][1] = nb++;
    }
    // The input variables to retype: Location in a mask, type pointer-to-vec4-float
    uint32_t moved[32]; int nmoved = 0, found = 0;
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCInput && w[i + 2] < bound && w[i + 1] == p_in_v4f) {
            uint32_t id = w[i + 2], l = loc[id];
            if (l < 32 && ((umask | smask) >> l & 1)) { cls[id] = (umask >> l & 1 ? 1 : 2) | 16; if (nmoved < 32) moved[nmoved++] = id; found |= 1u << l; }
        }
        i += len;
    }
    free(loc);
    if ((uint32_t)found != (umask | smask)) FAIL("inputs %#x are not vec4 float Input variables", (umask | smask) & ~(uint32_t)found);
    // Pass 2: copy, retyping. The globals up to the first function, minus the moved variables; then the new types and the variables; then the functions
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) if (need_c[c]) {
                if (mk_s[c]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_s[c], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_v[c]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_v[c], t_s[c], 4}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_p[c][0]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][0], SCInput, t_s[c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_p[c][1]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][1], SCInput, t_v[c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nmoved; k++) { uint32_t x[4] = {4u << 16 | OpVariable, p_in[(cls[moved[k]] & 3) - 1][1], moved[k], SCInput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1; uint32_t rt = w[i + 1], r = w[i + 2];
            // The chain's result: a component (pointer to float) or the vector itself (no index)
            if (r >= bound) FAIL("bad id");
            int vec = len == 4;
            if (!vec && len != 5) FAIL("an access chain of %u indexes into a retyped input", len - 4);
            (void)rt;
            cls[r] = (uint32_t)(c + 1) | (vec ? 16 : 0);
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = p_in[c][vec];
            i += len; continue;
        }
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1, vec = (cls[w[i + 3]] & 16) != 0;
            uint32_t ft = w[i + 1], r = w[i + 2], tmp = nb++;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = vec ? t_v[c] : t_s[c]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        // Any other use of a retyped variable or chain (a call argument, a copy): not understood, refused. Debug names, decorations and the entry point's interface are fine
        if (op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString && op != OpMemberDecorateString
            && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]] && op != OpVariable) FAIL("opcode %u uses a retyped input (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_subgroup_single: subgroup operations in a stage the device has none for. The Adreno 840 lists subgroup operations for the fragment and compute stages only
// (VkPhysicalDeviceSubgroupProperties::supportedStages 0xf0), and the translator emits them wherever the Maxwell code votes or shuffles: in Donkey Kong Bananza four geometry
// shaders (the terrain's culling shader 5d537f7630ec3671 among them) count each warp's surviving triangles with a ballot, let one lane add them to the chunk's indirect draw
// record and hand the base to the others with a shuffle. The driver takes the module and gives no meaningful lanes, so triangles were dropped or written over each other:
// black holes in the cave walls. Rewritten as subgroups of one invocation, which every warp-level algorithm must already handle (a warp with one active lane): the
// invocation's id is 0, the subgroup size 1, a ballot is its predicate in bit 0, the equal / less-or-equal / greater-or-equal masks are bit 0, the less / greater masks 0,
// a shuffle or broadcast returns its own value, all / any return the predicate, all-equal and elect true. Ballot counts, finds and inverse ballots are refused (none met)
enum { OpTypeBool = 20, OpConstantTrue = 41, OpConstant = 43, OpConstantComposite = 44, OpCompositeConstruct = 80, OpCopyObject = 83, OpSelect = 169,
       OpGNUElect = 333, OpGNUAll = 334, OpGNUAny = 335, OpGNUAllEqual = 336, OpGNUBroadcast = 337, OpGNUBroadcastFirst = 338, OpGNUBallot = 339,
       OpGNUShuffle = 345, OpGNUShuffleXor = 346, OpGNUShuffleUp = 347, OpGNUShuffleDown = 348 };
enum { DecBuiltIn = 11, BISubgroupSize = 36, BISubgroupLocalInvocationId = 41, BIEq = 4416, BIGe = 4417, BIGt = 4418, BILe = 4419, BILt = 4420 };
static int bi_kind(uint32_t b) {   // 1 = value 0 (id, lt, gt), 2 = value 1 (size, eq, le, ge), 0 = not a subgroup built-in
    switch (b) { case BISubgroupLocalInvocationId: case BILt: case BIGt: return 1; case BISubgroupSize: case BIEq: case BILe: case BIGe: return 2; default: return 0; }
}
uint32_t *vka_spv_subgroup_single(const uint32_t *w, size_t nw, size_t *out_nw, int *changed, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint8_t *bi = NULL, *chain = NULL; int32_t *cval = NULL; vec_t o = {0}; *changed = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3], nb = bound;
    bi = calloc(bound, 1); chain = calloc(bound, 1); cval = malloc((size_t)bound * 4);
    if (!bi || !chain || !cval) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) cval[i] = -1;
    uint32_t t_u32 = 0, t_v4u = 0, t_bool = 0; size_t first_fn = nw; int any = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 1] < bound && bi_kind(w[i + 3])) bi[w[i + 1]] = (uint8_t)bi_kind(w[i + 3]) | (w[i + 3] >= BIEq ? 4 : 0);   // bit 2: a vec4 mask
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32 && !w[i + 3]) t_u32 = w[i + 1];
        else if (op == OpTypeBool && len >= 2) t_bool = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] == 4 && w[i + 2] == t_u32 && t_u32) t_v4u = w[i + 1];
        else if (op == OpConstant && len >= 4 && w[i + 2] < bound) cval[w[i + 2]] = (int32_t)w[i + 3];
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        else if (op >= OpGNUElect && op <= OpGNUShuffleDown) any = 1;
        i += len;
    }
    for (uint32_t i = 0; i < bound && !any; i++) any = bi[i] != 0;
    if (!any) { free(bi); free(chain); free(cval); return NULL; }   // nothing to do
    if (!t_u32) FAIL("no 32-bit unsigned type");
    uint32_t mk_v4 = t_v4u ? 0 : nb++, mk_bool = t_bool ? 0 : nb++; if (!t_v4u) t_v4u = mk_v4; if (!t_bool) t_bool = mk_bool;
    uint32_t c0 = nb++, c1 = nb++, cv0 = nb++, cv1 = nb++, ctrue = nb++;   // uint 0, uint 1, uvec4(0), uvec4(1, 0, 0, 0), true
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[2] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(&o, x, 2)) FAIL("out of memory"); }
            if (mk_v4) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_v4, t_u32, 4}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1, 7u << 16 | OpConstantComposite, t_v4u, cv0, c0, c0, c0, c0,
                            7u << 16 | OpConstantComposite, t_v4u, cv1, c1, c0, c0, c0, 3u << 16 | OpConstantTrue, t_bool, ctrue};
            if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (i >= first_fn) {
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && bi[w[i + 3]]) {   // a component of a mask
                if (len != 5 || w[i + 4] >= bound || cval[w[i + 4]] < 0) FAIL("an access chain into a subgroup mask with a non-constant index");
                int k = bi[w[i + 3]] & 3; chain[w[i + 2]] = (uint8_t)(k == 2 && cval[w[i + 4]] == 0 ? 2 : 1);   // bit 0 only for eq / le / ge
                i += len; continue;   // dropped: its loads become constants
            }
            if (op == OpLoad && len >= 4 && w[i + 3] < bound && (bi[w[i + 3]] || chain[w[i + 3]])) {
                uint32_t v = chain[w[i + 3]] ? (chain[w[i + 3]] == 2 ? c1 : c0) : (bi[w[i + 3]] & 4) ? ((bi[w[i + 3]] & 3) == 2 ? cv1 : cv0) : ((bi[w[i + 3]] & 3) == 2 ? c1 : c0);
                uint32_t x[4] = {4u << 16 | OpCopyObject, w[i + 1], w[i + 2], v}; if (!put_n(&o, x, 4)) FAIL("out of memory");
                i += len; *changed = 1; continue;
            }
            if (op >= OpGNUElect && op <= OpGNUShuffleDown) {
                uint32_t rt = w[i + 1], r = w[i + 2], v;
                switch (op) {
                case OpGNUElect: case OpGNUAllEqual: v = ctrue; break;
                case OpGNUAll: case OpGNUAny: case OpGNUBroadcast: case OpGNUBroadcastFirst: case OpGNUShuffle: case OpGNUShuffleXor: case OpGNUShuffleUp: case OpGNUShuffleDown:
                    if (len < 5) FAIL("short subgroup instruction %u", op);
                    v = w[i + 4]; break;
                case OpGNUBallot: {
                    if (len < 5 || rt != t_v4u) FAIL("a ballot of an unexpected type");
                    uint32_t tmp = nb++;
                    uint32_t x[] = {6u << 16 | OpSelect, t_u32, tmp, w[i + 4], c1, c0, 7u << 16 | OpCompositeConstruct, t_v4u, r, tmp, c0, c0, c0};
                    if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
                    i += len; *changed = 1; continue; }
                default: FAIL("subgroup instruction %u not rewritten", op);
                }
                uint32_t x[4] = {4u << 16 | OpCopyObject, rt, r, v}; if (!put_n(&o, x, 4)) FAIL("out of memory");
                i += len; *changed = 1; continue;
            }
        }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(bi); free(chain); free(cval);
    *out_nw = o.n; return o.w;
fail:
    free(bi); free(chain); free(cval); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_gs_claim: a geometry shader that writes memory, made to write it once per primitive. The Adreno 840 runs a geometry shader several times for each primitive (three
// times for a triangle in and three vertices out, counted with an atomic add per gl_PrimitiveIDIn), each run executing the whole body: whatever the shader writes to memory
// is written that many times. Donkey Kong Bananza's terrain culling shader 5d537f7630ec3671 added each surviving triangle 3 times to its chunk's indirect draw record and wrote
// its indices 3 times, past the chunk's index buffer into the next chunk's: black holes in the cave walls. Vulkan says nothing of how many times a geometry shader runs, so the
// module claims its primitive first: one word per primitive in a storage buffer the replay binds at set 0 binding `binding` (VKA_GS_CLAIM_BINDING, zeroed for each draw), set by an atomic exchange
// at gl_PrimitiveIDIn; only the run that finds it 0 writes memory. Every store and copy into Uniform / StorageBuffer / PhysicalStorageBuffer / Image memory, image write and
// atomic is put under that condition (its block split around it); the other runs see an atomic's result as the memory's value (a compare-exchange its comparator, so a
// compare-exchange loop ends). What the shader emits is left alone: every run emits as before. Returns the count of memory writes (0: nothing to do, *out NULL), -1 with err
// on failure; with out and a count above 0, the rewritten module (malloc'd, *out_nw words)
enum { OpUndef = 1, OpExecutionMode = 16, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypeForwardPointer = 39,
       OpFunctionEnd = 56, OpFunctionParameter = 55, OpImageTexelPointer = 60, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpPtrAccessChain = 67,
       OpInBoundsPtrAccessChain = 70, OpImageWrite = 99, OpConvertUToPtr = 120, OpIEqual = 170, OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
       OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254, OpUnreachable = 255, OpTerminateInvocation = 4416,
       OpAtomicStore = 228, OpAtomicExchange = 229, OpAtomicCompareExchange = 230, OpAtomicCompareExchangeWeak = 231, OpAtomicXor = 242,
       OpAtomicFMinEXT = 5614, OpAtomicFMaxEXT = 5615, OpAtomicFAddEXT = 6035, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75 };
enum { SCUniform = 2, SCPrivate = 6, SCImage = 11, SCStorageBuffer = 12, SCPhysicalStorageBuffer = 5349, DecBufferBlock = 3, DecArrayStride = 6, DecOffset = 35,
       DecBinding = 33, DecDescriptorSet = 34, BIPrimitiveId = 7 };
static int sc_mem(uint32_t sc) { return sc == SCUniform || sc == SCImage || sc == SCStorageBuffer || sc == SCPhysicalStorageBuffer; }
static int is_term(uint32_t op) { return op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpKill || op == OpReturn || op == OpReturnValue || op == OpUnreachable || op == OpTerminateInvocation; }
static int is_annot(uint32_t op) { return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate || op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString; }
// One pass of the rewrite, run twice: the first records in rec where each split block of the input ends, the second gives that to the phis (last) that name such a block
// as their parent (the new ids are the same in both passes)
static int claim_pass(const uint32_t *w, size_t nw, uint32_t binding, const uint32_t *last, uint32_t *rec, vec_t *o, int *found, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t bound = w[3], nb = bound;
    uint32_t *psc = calloc(bound, 4), *ty = calloc(bound, 4);   // psc[pointer type]: storage class + 1; ty[id]: the type of a pointer-valued id
    if (!psc || !ty) FAIL("out of memory");
    uint32_t t_bool = 0, t_u32 = 0, t_i32 = 0, p_in_i32 = 0, p_priv_bool = 0, p_uni_u32 = 0, main_fn = 0, prim_var = 0;
    size_t first_fn = nw, first_annot = nw, ep_at = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) psc[w[i + 1]] = w[i + 2] + 1;
        if (op == OpTypePointer && len >= 4) {
            if (w[i + 2] == SCInput && w[i + 3] && w[i + 3] == t_i32) p_in_i32 = w[i + 1];
            if (w[i + 2] == SCPrivate && w[i + 3] && w[i + 3] == t_bool) p_priv_bool = w[i + 1];
            if (w[i + 2] == SCUniform && w[i + 3] && w[i + 3] == t_u32) p_uni_u32 = w[i + 1];
        }
        else if ((op == OpVariable || op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain || op == OpConvertUToPtr ||
                  op == OpBitcast || op == OpCopyObject || op == OpImageTexelPointer || op == OpFunctionParameter) && len >= 3 && w[i + 2] < bound) ty[w[i + 2]] = w[i + 1];
        if (op == OpTypeBool) t_bool = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpEntryPoint && len >= 3) { if (main_fn) FAIL("more than one entry point"); main_fn = w[i + 2]; ep_at = i; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == BIPrimitiveId) prim_var = w[i + 1];
        else if (op == OpExecutionMode && len >= 4 && w[i + 2] == 0 && w[i + 3] > 1) FAIL("%u invocations a primitive: they would share its claim", w[i + 3]);   // Invocations
        if (is_annot(op) && first_annot == nw) first_annot = i;
        if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!main_fn || first_fn == nw || !t_u32) FAIL("no entry point, function or 32-bit unsigned type");
    if (first_annot > first_fn) first_annot = first_fn;
    if (prim_var && (!p_in_i32 || ty[prim_var] != p_in_i32)) FAIL("a PrimitiveId input that is not a 32-bit int");
    // New ids: types missing, the claim buffer's types and variable, the constants, the guard variable
    uint32_t mk_bool = t_bool ? 0 : nb++, mk_i32 = t_i32 ? 0 : nb++; if (!t_bool) t_bool = mk_bool; if (!t_i32) t_i32 = mk_i32;
    uint32_t mk_pin = p_in_i32 && !mk_i32 ? 0 : nb++, mk_ppb = p_priv_bool && !mk_bool ? 0 : nb++, mk_pu = p_uni_u32 ? 0 : nb++;
    if (mk_pin) p_in_i32 = mk_pin; if (mk_ppb) p_priv_bool = mk_ppb; if (mk_pu) p_uni_u32 = mk_pu;
    uint32_t mk_prim = prim_var ? 0 : nb++; if (!prim_var) prim_var = mk_prim;
    uint32_t rta = nb++, st = nb++, pst = nb++, cvar = nb++, c0 = nb++, c1 = nb++, gvar = nb++;
    if (!put_n(o, w, 5)) FAIL("out of memory");
    uint32_t cur = 0, cur_orig = 0; int in_fn = 0, in_main_entry = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == ep_at && !mk_prim) { if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (i == ep_at) {   // the PrimitiveId input joins the interface
            if (!put(o, (len + 1) << 16 | OpEntryPoint) || !put_n(o, in + 1, len - 1) || !put(o, prim_var)) FAIL("out of memory");
            i += len; continue;
        }
        if (i == first_annot) {
            uint32_t x[] = {4u << 16 | OpDecorate, rta, DecArrayStride, 4, 3u << 16 | OpDecorate, st, DecBufferBlock, 5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0,
                            4u << 16 | OpDecorate, cvar, DecDescriptorSet, 0, 4u << 16 | OpDecorate, cvar, DecBinding, binding};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (mk_prim) { uint32_t y[] = {4u << 16 | OpDecorate, mk_prim, DecBuiltIn, BIPrimitiveId}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
        }
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(o, x, 2)) FAIL("out of memory"); }
            if (mk_i32) { uint32_t x[] = {4u << 16 | OpTypeInt, mk_i32, 32, 1}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pin) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pin, SCInput, t_i32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_ppb) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_ppb, SCPrivate, t_bool}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pu) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pu, SCUniform, t_u32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_prim) { uint32_t x[] = {4u << 16 | OpVariable, p_in_i32, mk_prim, SCInput}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {3u << 16 | OpTypeRuntimeArray, rta, t_u32, 3u << 16 | OpTypeStruct, st, rta, 4u << 16 | OpTypePointer, pst, SCUniform, st,
                            4u << 16 | OpVariable, pst, cvar, SCUniform, 4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1,
                            4u << 16 | OpVariable, p_priv_bool, gvar, SCPrivate};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (op == OpFunction) { in_fn = 1; in_main_entry = w[i + 2] == main_fn ? 1 : 0; }
        else if (op == OpFunctionEnd) in_fn = 0;
        if (op == OpLabel) { cur = cur_orig = w[i + 1]; if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (in_main_entry == 1 && op != OpVariable && cur) {   // the claim, after the entry block's variables
            uint32_t pid = nb++, ptr = nb++, old = nb++, first = nb++;
            uint32_t x[] = {4u << 16 | OpLoad, t_i32, pid, prim_var, 6u << 16 | OpAccessChain, p_uni_u32, ptr, cvar, c0, pid,
                            7u << 16 | OpAtomicExchange, t_u32, old, ptr, c1, c0, c1, 5u << 16 | OpIEqual, t_bool, first, old, c0, 3u << 16 | OpStore, gvar, first};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            in_main_entry = 2;
        }
        if (op == OpPhi && last) {   // a parent block that was split ends in its last piece
            if (!put_n(o, in, 3)) FAIL("out of memory");
            for (uint32_t k = 3; k + 1 < len; k += 2) { uint32_t par = in[k + 1]; if (!put(o, in[k]) || !put(o, par < bound && last[par] ? last[par] : par)) FAIL("out of memory"); }
            i += len; continue;
        }
        #define SC(id) ((id) < bound && ty[id] < bound && psc[ty[id]] ? psc[ty[id]] - 1 : 0u)
        int drop = 0, atomic = 0;
        if (in_fn && (op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpAtomicStore) && len >= 2 && sc_mem(SC(in[1]))) drop = 1;
        else if (in_fn && op == OpImageWrite) drop = 1;
        else if (in_fn && ((op >= OpAtomicExchange && op <= OpAtomicXor) || op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT) && len >= 4 && sc_mem(SC(in[3]))) atomic = 1;
        if (drop || atomic) {
            (*found)++;
            for (size_t j = i; j < nw;) { uint32_t oj = w[j] & 0xffff; if (oj == OpLoopMerge) FAIL("a memory write in a loop header block"); if (is_term(oj)) break; j += w[j] >> 16; }
            uint32_t g = nb++, a = nb++, l = nb++, fb = 0;
            if (atomic) {
                uint32_t sc = SC(in[3]);
                if ((op == OpAtomicCompareExchange || op == OpAtomicCompareExchangeWeak) && len >= 9) fb = in[8];
                else { fb = nb++;
                    if (sc == SCImage) { uint32_t x[] = {3u << 16 | OpUndef, in[1], fb}; if (!put_n(o, x, 3)) FAIL("out of memory"); }
                    else if (sc == SCPhysicalStorageBuffer) { uint32_t x[] = {6u << 16 | OpLoad, in[1], fb, in[3], 2, 4}; if (!put_n(o, x, 6)) FAIL("out of memory"); }   // Aligned 4
                    else { uint32_t x[] = {4u << 16 | OpLoad, in[1], fb, in[3]}; if (!put_n(o, x, 4)) FAIL("out of memory"); } }
            }
            uint32_t x[] = {4u << 16 | OpLoad, t_bool, g, gvar, 3u << 16 | OpSelectionMerge, l, 0, 4u << 16 | OpBranchConditional, g, a, l, 2u << 16 | OpLabel, a};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (atomic) { uint32_t r = in[2]; if (!put(o, in[0]) || !put(o, in[1]) || !put(o, nb) || !put_n(o, in + 3, len - 3)) FAIL("out of memory");
                uint32_t ra = nb++, y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l, 7u << 16 | OpPhi, in[1], r, ra, a, fb, cur};
                if (!put_n(o, y, sizeof y / 4)) FAIL("out of memory"); }
            else { if (!put_n(o, in, len)) FAIL("out of memory"); uint32_t y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
            cur = l; if (rec && cur_orig < bound) rec[cur_orig] = l;
            i += len; continue;
        }
        #undef SC
        if (!put_n(o, in, len)) FAIL("out of memory");
        i += len;
    }
    o->w[3] = nb;
    free(psc); free(ty);
    return 0;
fail:
    free(psc); free(ty);
    return -1;
#undef FAIL
}
int vka_spv_gs_claim(const uint32_t *w, size_t nw, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn) {
    if (out) *out = NULL;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return -1; }
    uint32_t *rec = calloc(w[3], 4); vec_t o = {0}; int found = 0;
    if (!rec) { snprintf(err, errn, "out of memory"); return -1; }
    if (claim_pass(w, nw, binding, NULL, rec, &o, &found, err, errn)) { free(rec); free(o.w); return -1; }
    if (found && out) {
        free(o.w); o = (vec_t){0}; int f2 = 0;
        if (claim_pass(w, nw, binding, rec, NULL, &o, &f2, err, errn)) { free(rec); free(o.w); return -1; }
        *out = o.w; *out_nw = o.n; o.w = NULL;
    }
    free(rec); free(o.w);
    return found;
}

// vka_spv_fout_retype: float colour outputs drawn into integer targets. On Maxwell / Ampere a colour output is a 32-bit register per component and the target's format decides
// how its bits are read: a pixel shader the translator types as float (its outputs computed with float instructions, or moved through float registers) still writes the bits
// it holds into a UINT / SINT target. Vulkan leaves a float output to an integer attachment undefined, and the library's MOD_UINT / MOD_UINT_MIXED variants exist only for
// shaders whose outputs come from integer bits or constants (vk_android_build.py ps_variants). So for a pipeline where a float output meets an integer attachment, the module's
// Output variables at those locations (a float or a float vector) become the unsigned / signed integer type of as many components, each store into one (whole, or a component
// through an access chain) stores the OpBitcast of its value, and each load of one is bit-cast back. The bits reach the target unchanged, as on the Switch.
// Mario Kart World's fragment dcb0504c844f1ecc (DrivePassShaderDefault, paired with task-less mesh programs: by their shape ShadingRateWrite) writes its UINT target 0 this way
enum { SCOutput = 3 };
uint32_t *vka_spv_fout_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL, *loc = NULL, *ty = NULL; vec_t o = {0};   // cls[id]: 1 uint / 2 int, bit 4 the whole variable (else a component chain); ty: for each float type id its components (1..4), 0 none
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if ((umask | smask) == 0 || (umask & smask)) { snprintf(err, errn, "no output, or an output both uint and int"); return NULL; }
    if (!(cls = calloc(bound, 4)) || !(loc = calloc(bound, 4)) || !(ty = calloc(bound, 4))) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, t_int[2] = {0}, t_ivec[2][5] = {{0}}, p_out_int[2][5] = {{0}};   // [uint/int][components]; the existing types
    uint32_t *ptee = calloc(bound, 4); if (!ptee) FAIL("out of memory");                // Output pointer type → its pointee
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) { free(ptee); FAIL("bad instruction at word %zu", i); }
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3];
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32 && w[i + 1] < bound) { t_f32 = w[i + 1]; ty[t_f32] = 1; }
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) t_int[w[i + 3] ? 1 : 0] = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] >= 2 && w[i + 3] <= 4 && w[i + 1] < bound) {
            if (w[i + 2] == t_f32 && t_f32) ty[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) if (w[i + 2] == t_int[c] && t_int[c]) t_ivec[c][w[i + 3]] = w[i + 1];
        }
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCOutput && w[i + 1] < bound) {
            ptee[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) { if (w[i + 3] == t_int[c] && t_int[c]) p_out_int[c][1] = w[i + 1]; for (int n = 2; n <= 4; n++) if (w[i + 3] == t_ivec[c][n] && t_ivec[c][n]) p_out_int[c][n] = w[i + 1]; }
        }
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    for (int c = 0; c < 2; c++) t_ivec[c][1] = t_int[c];
    // The output variables to retype: Location in a mask, a pointer to a float or float vector
    uint32_t moved[32], mcomp[32]; int nmoved = 0; uint32_t found = 0, need[2] = {0};   // need[c]: bit n = the integer type of n components (and its pointers) is used
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCOutput && w[i + 1] < bound && w[i + 2] < bound) {
            uint32_t id = w[i + 2], l = loc[id], pt = ptee[w[i + 1]], n = pt < bound ? ty[pt] : 0;
            if (l < 32 && ((umask | smask) >> l & 1)) {
                if (!n) { free(ptee); FAIL("output %u is not a float or float vector", l); }
                if (len > 4) { free(ptee); FAIL("output %u has an initializer", l); }   // (the translator emits none; re-emitted without it, unwritten components would change)
                if (nmoved == 32) { free(ptee); FAIL("too many outputs"); }
                int c = umask >> l & 1 ? 0 : 1;
                cls[id] = (uint32_t)(c + 1) | 16; mcomp[nmoved] = n; moved[nmoved++] = id;
                found |= 1u << l; need[c] |= 1u << n | 2u;   // (the scalar for component chains)
            }
        }
        i += len;
    }
    free(ptee);
    if (found != (umask | smask)) FAIL("outputs %#x are not float Output variables", (umask | smask) & ~found);
    // New ids for the types the module lacks
    uint32_t nb = bound, mk_t[2][5] = {{0}}, mk_p[2][5] = {{0}};
    for (int c = 0; c < 2; c++) for (int n = 1; n <= 4; n++) if (need[c] >> n & 1) {
        if (!t_ivec[c][n]) t_ivec[c][n] = mk_t[c][n] = nb++;
        if (!p_out_int[c][n]) p_out_int[c][n] = mk_p[c][n] = nb++;
    }
    if ((need[0] && !t_ivec[0][1]) || (need[1] && !t_ivec[1][1])) FAIL("no scalar type");
    // Pass 2: copy, retyping (the new types and the moved variables go right before the first function, as in vka_spv_vin_retype)
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) {
                if (mk_t[c][1]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_t[c][1], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 2; n <= 4; n++) if (mk_t[c][n]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_t[c][n], t_ivec[c][1], (uint32_t)n}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 1; n <= 4; n++) if (mk_p[c][n]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][n], SCOutput, t_ivec[c][n]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nmoved; k++) { uint32_t x[4] = {4u << 16 | OpVariable, p_out_int[(cls[moved[k]] & 3) - 1][mcomp[k]], moved[k], SCOutput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1; uint32_t r = w[i + 2];
            if (r >= bound) FAIL("bad id");
            if (len != 5 || !(cls[w[i + 3]] & 16)) FAIL("an access chain of %u indexes into a retyped output", len - 4);
            cls[r] = (uint32_t)(c + 1);   // a component: pointer to the integer scalar
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = p_out_int[c][1];
            i += len; continue;
        }
        if (op == OpStore && len >= 3 && w[i + 1] < bound && cls[w[i + 1]]) {
            uint32_t p = w[i + 1], c = (cls[p] & 3) - 1, n = 1;
            if (cls[p] & 16) { for (int k = 0; k < nmoved; k++) if (moved[k] == p) n = mcomp[k]; }
            uint32_t tmp = nb++;
            uint32_t x[4] = {4u << 16 | OpBitcast, t_ivec[c][n], tmp, w[i + 2]}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 2] = tmp;
            i += len; continue;
        }
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            uint32_t p = w[i + 3], c = (cls[p] & 3) - 1, n = 1, ft = w[i + 1], r = w[i + 2], tmp = nb++;
            if (cls[p] & 16) { for (int k = 0; k < nmoved; k++) if (moved[k] == p) n = mcomp[k]; }
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = t_ivec[c][n]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        if (op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString && op != OpMemberDecorateString
            && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]]) FAIL("opcode %u uses a retyped output (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls); free(loc); free(ty);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(loc); free(ty); free(o.w);
    return NULL;
#undef FAIL
}

// A pixel shader that may keep early depth / stencil tests once its pool reads are marked (vka_spv_pool_mark's atomic turns them off on drivers that see a side effect):
// asked of the module before the marks. No FragDepth / SampleMask / FragStencilRefEXT variable, no store of its own (OpImageWrite, an atomic, OpStore or OpCopyMemory
// into a StorageBuffer, Uniform or PhysicalStorageBuffer pointer), and no discard (OpKill, OpTerminateInvocation, OpDemoteToHelperInvocation) except one with the alpha
// test (alpha_test: the entry has DMSH_VK_F_ALPHA_TEST, and the module its specialization constant spec_alpha), which a pipeline without the test never reaches.
// 1 = eligible when the pipeline's alpha test is off (nxnative: marked shaders declare [[early_fragment_tests]]; Ocarina of Time, 3,130 of 4,754 marked pixel shaders)
int vka_spv_early_ok(const uint32_t *w, size_t nw, int alpha_test, uint32_t spec_alpha) {
    enum { OpTypePointer = 32, OpVariable = 59, OpStore = 62, OpCopyMemory = 63, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67, OpImageWrite = 99,
           OpConvertUToPtr = 120, OpKill = 252, OpTerminateInvocation = 4416, OpDemote = 5380, DecBuiltIn = 11, DecSpecId = 1,
           BIFragDepth = 22, BISampleMask = 20, BIFragStencilRef = 5014, SCUniform = 2, SCStorageBuffer = 12, SCPhysical = 5349 };
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; uint32_t *sc = calloc(bound ? bound : 1, 4);   // per id: the storage class a pointer (type, variable or chain) leads to, +1 (0 = none)
    if (!sc) return 0;
    int kills = 0, spec = 0, bad = 0;
    for (size_t i = 5; i < nw && !bad;) {
        uint32_t op = w[i] & 0xffff, ln = w[i] >> 16;
        if (!ln || i + ln > nw) { bad = 1; break; }
        if (op == OpDecorate && ln >= 4 && w[i + 2] == DecBuiltIn && (w[i + 3] == BIFragDepth || w[i + 3] == BISampleMask || w[i + 3] == BIFragStencilRef)) bad = 1;
        else if (op == OpDecorate && ln >= 4 && w[i + 2] == DecSpecId && w[i + 3] == spec_alpha) spec = 1;
        else if (op == OpTypePointer && ln == 4 && w[i + 1] < bound) sc[w[i + 1]] = w[i + 2] + 1;
        else if (op == OpVariable && ln >= 4 && w[i + 2] < bound) sc[w[i + 2]] = w[i + 3] + 1;
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain) && ln >= 4 && w[i + 2] < bound && w[i + 3] < bound)
            sc[w[i + 2]] = sc[w[i + 3]] ? sc[w[i + 3]] : (w[i + 1] < bound ? sc[w[i + 1]] : 0);
        else if (op == OpConvertUToPtr && ln >= 4 && w[i + 2] < bound) sc[w[i + 2]] = SCPhysical + 1;
        else if (op == OpKill || op == OpTerminateInvocation || op == OpDemote) kills++;
        else if (op == OpImageWrite || (op >= 227 && op <= 242) || op == 4417 || op == 5614 || op == 5615) bad = 1;   // OpAtomicLoad … OpAtomicXor, the float atomics
        else if ((op == OpStore || op == OpCopyMemory) && ln >= 3 && w[i + 1] < bound) {
            uint32_t c = sc[w[i + 1]];
            if (c == SCStorageBuffer + 1 || c == SCUniform + 1 || c == SCPhysical + 1) bad = 1;
        }
        i += ln;
    }
    free(sc);
    if (bad) return 0;
    return kills == 0 || (kills == 1 && alpha_test && spec);
}
// The module with OpExecutionMode EarlyFragmentTests on each fragment entry point (after the entry points and execution modes); NULL with err set when there is none
uint32_t *vka_spv_early_tests(const uint32_t *w, size_t nw, size_t *out_nw, char *err, size_t errn) {
    enum { OpEntryPoint = 15, OpExecutionMode = 16, OpExecutionModeId = 331, ModelFragment = 4, ModeEarlyTests = 9 };
    *out_nw = 0; if (errn) err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t ent[8]; int nent = 0; size_t after = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, ln = w[i] >> 16;
        if (!ln || i + ln > nw) { snprintf(err, errn, "bad instruction at word %zu", i); return NULL; }
        if (op == OpEntryPoint && ln >= 3 && w[i + 1] == ModelFragment && nent < 8) ent[nent++] = w[i + 2];
        if (op == OpEntryPoint || op == OpExecutionMode || op == OpExecutionModeId) after = i + ln;
        else if (after) break;   // (the execution modes come right after the entry points)
        if (op == OpExecutionMode && ln >= 3 && w[i + 2] == ModeEarlyTests) { snprintf(err, errn, "already has EarlyFragmentTests"); return NULL; }
        i += ln;
    }
    if (!nent || !after) { snprintf(err, errn, "no fragment entry point"); return NULL; }
    vec_t o = {0};
    if (!put_n(&o, w, after)) goto nomem;
    for (int k = 0; k < nent; k++) { uint32_t m[3] = {3u << 16 | OpExecutionMode, ent[k], ModeEarlyTests}; if (!put_n(&o, m, 3)) goto nomem; }
    if (!put_n(&o, w + after, nw - after)) goto nomem;
    *out_nw = o.n; return o.w;
nomem:
    free(o.w); snprintf(err, errn, "no memory"); return NULL;
}
