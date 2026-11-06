// DM_VK_NANSCAN=1: a diagnostic that finds the pass that first writes NaN or infinite texels. After every render pass, each float colour attachment's level / layer is
// scanned by a compute shader (selftest/nanscan.comp) that counts NaN and infinite texels and keeps the first one's position; when the submission is done the completion
// thread logs each pass with any, with its frame, its place in the submission, the attachment (serial, format, size) and the vertex / fragment shaders of its draws.
// DM_VK_NANSCAN=2 also splits long passes to find the draw: the Vulkan pass is ended (and reopened by the next draw, its attachments loaded) after every
// DM_VK_NANSCAN_STEP draws (default 64), in passes into the image DM_VK_NANSCAN_SERIAL (default any), of the game's pass whose index is in DM_VK_NANSCAN_DRAWS=<from>-<to> (default all), at most MAXSPLIT times a submission,
// and only the scans after which an attachment has more NaN / infinite texels than at its previous scan in the submission are logged, with the pass's draw range.
// Bisect with a coarse step over the whole pass, then step 1 over the range it names. Costs a dispatch per attachment per pass (per split with 2): for finding a
// cause, not for play
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_selftest_spv.h"
#include <stdlib.h>
#include <string.h>

enum { NBATCH = 16, MAXR = 1024, NKEY = 8, MAXSPLIT = 256 };
typedef struct { uint32_t nan, inf, first, pad; } res_t;
typedef struct { uint64_t frame, serial; uint32_t pass, fmt, w, h, lv, layer, d0, ndraws, nkeys; uint64_t vs[NKEY], fs[NKEY]; int bind, tid; } rec_t;
typedef struct { rec_t r[MAXR]; int n; res_t *res; VkDeviceSize off; } batch_t;
static batch_t g_b[NBATCH]; static unsigned g_bi; static vka_span g_res;
static VkPipeline g_pipe, g_pipe3, g_fetch; static VkPipelineLayout g_lay; static VkDescriptorSetLayout g_dsl;
static uint32_t g_pass; static uint64_t g_pvs[NKEY], g_pfs[NKEY]; static uint32_t g_pn, g_pdraws, g_d0;   // g_pdraws: draws of the game's pass so far, g_d0: the segment's first
static uint64_t g_probe; static int g_split, g_nsplit; static uint32_t g_step = 64, g_from, g_to = UINT32_MAX; static uint64_t g_only;
static _Atomic int g_lines;

int vka_nan_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_VK_NANSCAN"); v = e && atoi(e) > 0 && V.push_desc ? atoi(e) : 0;
    if (v >= 2) {
        const char *s = getenv("DM_VK_NANSCAN_STEP"), *r = getenv("DM_VK_NANSCAN_DRAWS"); if (s && atoi(s) > 0) g_step = (uint32_t)atoi(s);
        const char *o = getenv("DM_VK_NANSCAN_SERIAL"); if (o) g_only = strtoull(o, NULL, 10);
        if (r) { char *x; g_from = (uint32_t)strtoul(r, &x, 10); if (*x == '-') g_to = (uint32_t)strtoul(x + 1, NULL, 10); }
        VLOG("DM_VK_NANSCAN=2: passes split every %u draws in draws %u-%u of a pass%s, at most %d splits a submission", g_step, g_from, g_to, g_only ? " into DM_VK_NANSCAN_SERIAL" : "", MAXSPLIT);
    } else if (v) VLOG("DM_VK_NANSCAN: every pass's float attachments are scanned for NaN / infinite texels");
    return v;
}
// DM_VK_NANSCAN=2, after a draw: 1 = end the Vulkan pass now for a scan (the game's pass goes on)
int vka_nan_split(vka_img *const *ims, int n) {
    uint32_t i = g_pdraws;   // the draws of the game's pass so far
    if (i <= g_from || i > g_to || (i - g_from) % g_step) return 0;
    if (g_nsplit >= MAXSPLIT) { static _Atomic int n; if (!atomic_exchange(&n, 1)) VLOG("NaN scan: %d splits in one submission, no more in it (at draw %u of pass %u)", MAXSPLIT, i, g_pass); return 0; }
    if (g_only) { int k = 0; while (k < n && !(ims[k] && ims[k]->serial == g_only)) k++; if (k == n) return 0; }   // DM_VK_NANSCAN_SERIAL: only passes into that image
    g_nsplit++; g_split = 1; return 1;
}
static int ready(void) {
    static int st; if (st) return st > 0;
    st = -1;
    if (!vka_static((VkDeviceSize)NBATCH * MAXR * sizeof(res_t), V.sb_align, &g_res)) return 0;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof SPV_NANSCAN_COMP, .pCode = SPV_NANSCAN_COMP};
    VkResult r;
    if ((r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_dsl)) || (r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_lay))) { VRED("NaN scan: %d", r); return 0; }
    for (int i = 0; i < 3; i++) {   // 2D-array views (attachments, 2D / cube inputs), 3D inputs, the ASTC check's fetch
        VkShaderModule m = VK_NULL_HANDLE;
        if (i == 1) { mi.codeSize = sizeof SPV_NANSCAN3_COMP; mi.pCode = SPV_NANSCAN3_COMP; }
        if (i == 2) { mi.codeSize = sizeof SPV_FETCH_COMP; mi.pCode = SPV_FETCH_COMP; }
        if ((r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m))) { VRED("NaN scan: %d", r); return 0; }
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_lay};
        r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, i == 2 ? &g_fetch : i ? &g_pipe3 : &g_pipe);
        vk.vkDestroyShaderModule(V.dev, m, NULL);
        if (r) { VRED("NaN scan pipeline: %d", r); return 0; }
    }
    for (int i = 0; i < NBATCH; i++) { g_b[i].res = (res_t *)(g_res.ptr + (size_t)i * MAXR * sizeof(res_t)); g_b[i].off = g_res.off + (VkDeviceSize)i * MAXR * sizeof(res_t); }
    st = 1; return 1;
}
void vka_nan_draw(uint64_t vs, uint64_t fs) {
    g_pdraws++;   // counted before the draw records: a draw skipped after this still has its index
    for (uint32_t i = 0; i < g_pn; i++) if (g_pvs[i] == vs && g_pfs[i] == fs) return;
    if (g_pn < NKEY) { g_pvs[g_pn] = vs; g_pfs[g_pn] = fs; g_pn++; }
}
uint64_t vka_frame(void);
// The ASTC check (once per run with DM_VK_NANSCAN): an 8x8 ASTC 4x4 texture of four void-extent (constant colour) blocks, sampled by the device and logged. A void-extent block's
// colour is UNORM16 when its bit 9 (dynamic range) is 0 and FP16 when it is 1 (ASTC specification, "Void-Extent Blocks")
static const uint16_t g_vcol[4][4] = {{0x1212, 0x1212, 0x1212, 0xffff}, {0x8000, 0x4000, 0xc000, 0x8000}, {0x3c00, 0x3800, 0x3400, 0x3c00}, {0xffff, 0x0000, 0xffff, 0xffff}};
static const int g_vhdr[4] = {0, 0, 1, 0};
static vka_span g_fetch_out; static int g_astc_state;   // 1 = recorded, report at the next commit
static void astc_report(void *arg) {
    (void)arg; const float *t = (const float *)g_fetch_out.ptr;
    for (int b = 0; b < 4; b++) {
        const float *v = t + 4 * ((b / 2) * 4 * 8 + (b % 2) * 4);   // the block's first texel in the 8x8 texture
        VLOG("NaN scan: ASTC check: void-extent block %d (%s, colour %04x %04x %04x %04x) samples as (%g, %g, %g, %g)", b, g_vhdr[b] ? "HDR: FP16" : "LDR: UNORM16",
             g_vcol[b][0], g_vcol[b][1], g_vcol[b][2], g_vcol[b][3], v[0], v[1], v[2], v[3]);
    }
}
static void astc_check(VkCommandBuffer cb) {
    static uint8_t blocks[4 * 16]; static tex_t t;
    for (int b = 0; b < 4; b++) {
        uint64_t lo = 0x1fc | (uint64_t)g_vhdr[b] << 9 | 3ull << 10 | 0xfffffffffffffull << 12, hi = 0;   // extent all ones: the whole texture
        for (int c = 0; c < 4; c++) hi |= (uint64_t)g_vcol[b][c] << (16 * c);
        memcpy(blocks + 16 * b, &lo, 8); memcpy(blocks + 16 * b + 8, &hi, 8);
    }
    memset(&t, 0, sizeof t);
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = 0x79; t.d.width = 8; t.d.height = 8; t.d.depth = 1; t.d.levels = 1; t.d.flags = NVN_TEXTURE_FLAGS_LINEAR; t.d.stride = 32;
    nvn_tex_layout(&t.d, &t.L); t.mem = blocks; t.serial = nvn_new_serial();
    vka_img *im = vka_tex_get(&t, cb);
    if (!im || !vka_static(64 * 16, V.sb_align, &g_fetch_out)) { VLOG("NaN scan: ASTC check: no test texture"); return; }
    memset(g_fetch_out.ptr, 0xff, 64 * 16);
    VkImageView v = vka_view(im, im->f.view, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, NULL);
    if (!v) return;
    VkDescriptorImageInfo ii = {vka_samp_default(), v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {g_fetch_out.buf, g_fetch_out.off, 64 * 16};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    uint32_t pc[4] = {8, 8, 0, 0};
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_fetch);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_lay, 0, 2, wr); vka_dyn_invalidate();   // the draws' last push is no longer bound (vka_draw.c push_same)
    vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(cb, 1, 1, 1);
    vka_barrier(cb);
    g_astc_state = 1;
}
// One dispatch: level lv, layer l0 of im into the batch's next result
static int scan_one(VkCommandBuffer cb, batch_t *b, vka_img *im, uint32_t lv, uint32_t l0, uint32_t pass, int bind, int tid) {
    if (b->n == MAXR) return 0;
    int v3 = im->d.target == NVN_TEXTURE_TARGET_3D;   // a whole level of a 3D texture (inputs only)
    VkImageView v = vka_view(im, im->f.view, v3 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, v3 ? 0 : l0, 1, NULL);
    if (!v) return 0;
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, dz = v3 ? (im->depth >> lv ? im->depth >> lv : 1) : 1;
    int k = b->n++;
    b->res[k] = (res_t){0, 0, 0xffffffffu, 0};
    b->r[k] = (rec_t){vka_frame(), im->serial, pass, (uint32_t)im->d.format, w, h, lv, l0, g_d0, g_pdraws, g_pn, {0}, {0}, bind, tid};
    memcpy(b->r[k].vs, g_pvs, sizeof g_pvs); memcpy(b->r[k].fs, g_pfs, sizeof g_pfs);
    VkDescriptorImageInfo ii = {vka_samp_default(), v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {g_res.buf, b->off + (VkDeviceSize)k * sizeof(res_t), sizeof(res_t)};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    uint32_t pc[4] = {w, h, v3 ? dz : 0, 0};
    vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v3 ? g_pipe3 : g_pipe);
    vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_lay, 0, 2, wr); vka_dyn_invalidate();   // the draws' last push is no longer bound (vka_draw.c push_same)
    vk.vkCmdPushConstants(cb, g_lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vk.vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, dz);
    return 1;
}
// DM_VK_NANSCAN_FS=<fragment shader key>[,<key>…] (with DM_VK_NANSCAN=2; at most 4 keys): the first 4 draws of each frame with each of those fragment shaders have
// their sampled float textures scanned before it (every level and layer, outside the pass): which input holds NaN / infinite texels. 1 = this draw is one (the caller ends the pass and
// calls vka_nan_inputs)
enum { NPROBE = 4 };
static uint64_t g_probes[NPROBE], g_cur; static int g_nprobe;
int vka_nan_probe(uint64_t fs) {
    static uint64_t last[NPROBE]; static int init;
    if (!init) {
        init = 1; const char *e = getenv("DM_VK_NANSCAN_FS");
        for (char *x; e && *e && g_nprobe < NPROBE; e = *x == ',' ? x + 1 : x) { uint64_t k = strtoull(e, &x, 16); if (x == e) break; last[g_nprobe] = ~0ull; g_probes[g_nprobe++] = k; }
        if (g_nprobe) g_probe = g_probes[0];
    }
    static int nin[NPROBE];   // draws probed in the frame last[i] (the first 4 of each frame and key: a text box draws one sheet after another)
    int i = 0; while (i < g_nprobe && (!fs || fs != g_probes[i])) i++;
    if (i == g_nprobe) return 0;
    if (last[i] != vka_frame()) { last[i] = vka_frame(); nin[i] = 0; }
    if (nin[i] >= 4) return 0;
    nin[i]++; g_cur = fs; return 1;
}
// DM_VK_NANSCAN_GPUDUMP=<n> (live; with DM_VK_NANSCAN=2 and DM_VK_NANSCAN_FS): the probed draw's sampled colour textures as the GPU holds them before it (level 0, layer 0 or a 3D texture's every slice, uncompressed
// formats), each copied into a host-visible buffer of its own and written to DM_TEMP as gpu-<fragment key>-s<serial>-f<frame>-<w>x<h>-vk<VkFormat>.blk (rows packed)
// once the submission is done: what a render target the GPU drew holds, which vka_tex_id_dump (guest storage) cannot show. The first 3 frames of each key, 48 files at most
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; uint64_t key, serial, frame; uint32_t w, h, d, vkf; int after; VkDeviceSize n; } gdump_t;
static gdump_t *g_gd[64]; static int g_ngd, g_after;
static void gdump_write(void *arg) {   // the completion thread, after the submission's fence
    gdump_t *g = arg; char fn[512];
    snprintf(fn, sizeof fn, "%s/gpu-%016llx-%s-s%llu-f%llu-%ux%ux%u-vk%u.blk", getenv("DM_TEMP") ? getenv("DM_TEMP") : "/data/local/tmp", (unsigned long long)g->key,
             g->after ? "after" : "in", (unsigned long long)g->serial, (unsigned long long)g->frame, g->w, g->h, g->d, g->vkf);
    FILE *f = fopen(fn, "wb"); if (f) { fwrite(g->map, 1, (size_t)g->n, f); fclose(f); }
    VLOG("NaN scan: GPU content of serial %llu (%ux%ux%u VkFormat %u, %s a draw of %016llx, frame %llu) %s %s", (unsigned long long)g->serial, g->w, g->h, g->d, g->vkf,
         g->after ? "target, after" : "input, before", (unsigned long long)g->key, (unsigned long long)g->frame, f ? "written to" : "could not be written to", fn);
    vk.vkUnmapMemory(V.dev, g->mem); vk.vkDestroyBuffer(V.dev, g->buf, NULL); vk.vkFreeMemory(V.dev, g->mem, NULL); free(g);
}
static void gdump_record(VkCommandBuffer cb, vka_img *const *ims, int n) {
    static _Atomic int total; static uint64_t keys[NPROBE], kf[NPROBE]; static int frames[NPROBE], armed;
    int arm = vka_nan_gpudump();
    if (arm != armed) { armed = arm; atomic_store(&total, 0); memset(keys, 0, sizeof keys); memset(kf, 0, sizeof kf); memset(frames, 0, sizeof frames); }
    int ki = 0; while (ki < NPROBE && keys[ki] && keys[ki] != g_cur) ki++;
    if (ki == NPROBE) return;
    if (!keys[ki]) keys[ki] = g_cur;
    if (kf[ki] != vka_frame()) { if (frames[ki] >= 3) return; frames[ki]++; kf[ki] = vka_frame(); }
    int bar = 0;
    for (int i = 0; i < n && g_ngd < 64; i++) {
        vka_img *im = ims[i];
        if (!im || !im->inited || im->samples != 1 || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->f.bw != 1 || im->f.bh != 1 || !im->f.bpb) continue;
        uint32_t d = im->d.target == NVN_TEXTURE_TARGET_3D && im->depth > 1 ? im->depth : 1;   // a 3D texture whole (Ocarina of Time's fog volume, 128x72x96)
        VkDeviceSize sz = (VkDeviceSize)im->w * im->h * d * im->f.bpb; if (!sz || sz > (64u << 20) || atomic_fetch_add(&total, 1) >= 48) continue;
        gdump_t *g = calloc(1, sizeof *g); if (!g) continue;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vk.vkCreateBuffer(V.dev, &bi, NULL, &g->buf)) { free(g); continue; }
        VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, g->buf, &mr);
        uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
        if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &g->mem) || vk.vkBindBufferMemory(V.dev, g->buf, g->mem, 0) || vk.vkMapMemory(V.dev, g->mem, 0, VK_WHOLE_SIZE, 0, &g->map)) {
            if (g->mem) vk.vkFreeMemory(V.dev, g->mem, NULL);
            vk.vkDestroyBuffer(V.dev, g->buf, NULL); free(g); continue;
        }
        if (!bar) { vka_barrier(cb); bar = 1; }
        VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {im->w, im->h, d}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g->buf, 1, &r);
        g->key = g_cur; g->serial = im->serial; g->frame = vka_frame(); g->w = im->w; g->h = im->h; g->d = d; g->vkf = (uint32_t)im->f.vk; g->n = sz; g->after = g_after;
        g_gd[g_ngd++] = g;
    }
    if (bar) {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vka_barrier(cb);   // the draws after it see the image as before
    }
}
// DM_VK_NANSCAN_GPUDUMP: queries around each probed draw, read when its submission is done: samples passed (occlusion) and, where the device has them, the pipeline
// statistics (vertices, primitives, vertex shader invocations, primitives in and out of clipping, fragment shader invocations). Which stage loses a draw that writes nothing
enum { NQS = 64 };
static VkQueryPool g_qocc, g_qps; static int g_qok = -1, g_qnext; static uint64_t g_qkey[NQS], g_qframe[NQS];
static PFN_vkCmdBeginQuery g_qbegin; static PFN_vkCmdEndQuery g_qend; static PFN_vkCmdResetQueryPool g_qreset; static PFN_vkGetQueryPoolResults g_qget;
static int q_init(void) {
    if (g_qok >= 0) return g_qok;
    g_qok = 0;
    g_qbegin = (PFN_vkCmdBeginQuery)vk.vkGetDeviceProcAddr(V.dev, "vkCmdBeginQuery"); g_qend = (PFN_vkCmdEndQuery)vk.vkGetDeviceProcAddr(V.dev, "vkCmdEndQuery");
    g_qreset = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool"); g_qget = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    if (!g_qbegin || !g_qend || !g_qreset || !g_qget || !create) return 0;
    VkQueryPoolCreateInfo oc = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = NQS};
    if (create(V.dev, &oc, NULL, &g_qocc)) { VLOG("NaN scan: no occlusion query pool"); return 0; }
    VkQueryPoolCreateInfo pc = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = NQS,
        .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                              VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
    if (create(V.dev, &pc, NULL, &g_qps)) { g_qps = VK_NULL_HANDLE; VLOG("NaN scan: no pipeline statistics query pool (occlusion only)"); }
    g_qok = 1; return 1;
}
int vka_nan_query_reset(VkCommandBuffer cb, uint64_t key) {   // outside a pass: a fresh slot for the probed draw, or -1
    if (!q_init() || g_qnext >= NQS) return -1;
    int s = g_qnext++;
    g_qreset(cb, g_qocc, (uint32_t)s, 1); if (g_qps) g_qreset(cb, g_qps, (uint32_t)s, 1);
    g_qkey[s] = key; g_qframe[s] = vka_frame(); return s;
}
void vka_nan_query_begin(VkCommandBuffer cb, int s) { g_qbegin(cb, g_qocc, (uint32_t)s, VK_QUERY_CONTROL_PRECISE_BIT); if (g_qps) g_qbegin(cb, g_qps, (uint32_t)s, 0); }
static int g_qpend[NQS]; static int g_nqpend;
void vka_nan_query_end(VkCommandBuffer cb, int s) { if (g_qps) g_qend(cb, g_qps, (uint32_t)s); g_qend(cb, g_qocc, (uint32_t)s); if (g_nqpend < NQS) g_qpend[g_nqpend++] = s; }
static void q_report(void *arg) {   // the completion thread
    int s = (int)(intptr_t)arg; uint64_t occ = 0, ps[6] = {0};
    VkResult r1 = g_qget(V.dev, g_qocc, (uint32_t)s, 1, sizeof occ, &occ, sizeof occ, VK_QUERY_RESULT_64_BIT), r2 = g_qps ? g_qget(V.dev, g_qps, (uint32_t)s, 1, sizeof ps, ps, sizeof ps, VK_QUERY_RESULT_64_BIT) : VK_ERROR_FEATURE_NOT_PRESENT;
    VLOG("NaN scan: probed draw of %016llx (frame %llu), the GPU's count: samples passed %llu%s; vertices %llu, primitives %llu, vertex shader invocations %llu, clipping in %llu / out %llu, fragment shader invocations %llu%s",
         (unsigned long long)g_qkey[s], (unsigned long long)g_qframe[s], (unsigned long long)occ, r1 ? " (not available)" : "", (unsigned long long)ps[0], (unsigned long long)ps[1], (unsigned long long)ps[2],
         (unsigned long long)ps[3], (unsigned long long)ps[4], (unsigned long long)ps[5], r2 ? " (statistics not available)" : "");
}
// Live (VKA_ENV): 0 off; each new non-zero value arms the dumps again (gdump_record's per-key frame counts and file total start over)
int vka_nan_gpudump(void) { return VKA_ENV("DM_VK_NANSCAN_GPUDUMP", 0); }
void vka_nan_dump_targets(VkCommandBuffer cb, vka_img *const *ims, int n) { g_after = 1; gdump_record(cb, ims, n); g_after = 0; }
void vka_nan_inputs(VkCommandBuffer cb, vka_img *const *ims, const int *binds, const int *tids, int n) {
    batch_t *b = &g_b[g_bi % NBATCH];
    if (vka_nan_gpudump()) gdump_record(cb, ims, n);
    if (!ready()) return;
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->f.kind != 0 || im->samples > 1 || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) continue;
        uint32_t nl = im->d.target == NVN_TEXTURE_TARGET_3D ? 1 : im->layers;
        for (uint32_t lv = 0; lv < im->levels && lv < 14; lv++) for (uint32_t l = 0; l < nl && l < 6; l++) any |= scan_one(cb, b, im, lv, l, g_pass, binds[i], tids[i]);
    }
    if (any) vka_barrier(cb);
    static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VLOG("NaN scan: frame %llu: %d inputs of fragment shader %016llx scanned before its first draw (only those with NaN / infinite texels are logged)", (unsigned long long)vka_frame(), n, (unsigned long long)g_probe);
}
// After a pass ends (its writes made visible by the caller's barrier): one dispatch per float colour attachment
void vka_nan_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n) {
    batch_t *b = &g_b[g_bi % NBATCH];
    int split = g_split; g_split = 0;
    uint32_t pass = split ? g_pass : g_pass++;
    if (!ready()) return;
    if (!g_astc_state) { g_astc_state = -1; astc_check(cb); }
    int any = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->f.kind != 0 || im->samples > 1 || im->d.target == NVN_TEXTURE_TARGET_3D || !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || b->n == MAXR) continue;
        any |= scan_one(cb, b, im, lvs[i], l0s[i], pass, -1, 0);
    }
    if (any) vka_barrier(cb);
    g_pn = 0;
    if (split) g_d0 = g_pdraws; else { g_pdraws = 0; g_d0 = 0; }
}
static void report(void *arg) {
    batch_t *b = arg;
    for (int k = 0; k < b->n; k++) {
        const res_t *r = &b->res[k]; const rec_t *c = &b->r[k];
        if (!r->nan && !r->inf) continue;
        if (c->bind >= 0) {
            if (atomic_fetch_add(&g_lines, 1) >= 200) return;
            VLOG("NaN scan: frame %llu pass %u (fragment %016llx): input binding %d texture ID %d serial %llu (format %#x %ux%u level %u layer %u): %u NaN + %u infinite texels, first at (%u, %u)",
                 (unsigned long long)c->frame, c->pass, (unsigned long long)g_probe, c->bind, c->tid, (unsigned long long)c->serial, c->fmt, c->w, c->h, c->lv, c->layer, r->nan, r->inf, r->first & 0xffff, r->first >> 16);
            continue;
        }
        if (vka_nan_on() >= 2) {   // only where the count of this attachment grew since its previous scan in this submission
            int j = k - 1; while (j >= 0 && (b->r[j].serial != c->serial || b->r[j].lv != c->lv || b->r[j].layer != c->layer)) j--;
            if (j >= 0 && b->res[j].nan + b->res[j].inf >= r->nan + r->inf) continue;
        }
        if (atomic_fetch_add(&g_lines, 1) >= 200) return;
        char keys[400] = ""; size_t o = 0;
        for (uint32_t i = 0; i < c->nkeys && o < sizeof keys - 40; i++) o += (size_t)snprintf(keys + o, sizeof keys - o, " %016llx/%016llx", (unsigned long long)c->vs[i], (unsigned long long)c->fs[i]);
        VLOG("NaN scan: frame %llu pass %u: %u NaN + %u infinite texels in serial %llu (format %#x %ux%u level %u layer %u), first at (%u, %u); draws %u-%u of the pass, vertex/fragment%s%s",
             (unsigned long long)c->frame, c->pass, r->nan, r->inf, (unsigned long long)c->serial, c->fmt, c->w, c->h, c->lv, c->layer, r->first & 0xffff, r->first >> 16, c->d0, c->ndraws, keys, c->nkeys == NKEY ? " …" : "");
    }
}
void vka_nan_commit(uint64_t fence) {   // the submission's batch is read when fence is done; the next submission starts a new batch
    batch_t *b = &g_b[g_bi % NBATCH];
    for (int i = 0; i < g_ngd; i++) vka_after(fence, gdump_write, g_gd[i]);   // DM_VK_NANSCAN_GPUDUMP: written once the GPU copied them
    g_ngd = 0;
    for (int i = 0; i < g_nqpend; i++) vka_after(fence, q_report, (void *)(intptr_t)g_qpend[i]);   // and the probed draws' queries
    g_nqpend = 0;
    if (b->n) vka_after(fence, report, b);
    if (g_astc_state == 1) { g_astc_state = 2; vka_after(fence, astc_report, NULL); }
    g_bi++; g_b[g_bi % NBATCH].n = 0; g_pass = 0; g_nsplit = 0;
}
#endif
