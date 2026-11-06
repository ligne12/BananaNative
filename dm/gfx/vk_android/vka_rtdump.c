// DM_VK_RTDUMP=<n> (Android, live: each new value arms it again): the colour targets of every render pass of the next n frames, each blitted on the GPU into a small
// RGBA16F image (at most RTD_W texels wide, the aspect kept) and read back once its submission is done. One "rt dump" line a target: frame, pass, the pass's draws and
// first shader pairs, serial, size, format; mean and largest luminance, the share of texels brighter than 1 and 4, NaN / infinite texels. The thumbnail goes to DM_TEMP as
// rt-f<frame>-p<pass>-a<attachment>-s<serial>-<w>x<h>.rgba16f (rows packed). Two frames, a right one and a wrong one, compared pass by pass show where a wrong picture
// begins (Ocarina of Time's white objects after a save loads)
#include "vka.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t vka_frame(void);
enum { RTD_W = 320, RTD_KEYS = 4, RTD_MAX = 800 };
typedef struct {
    VkImage img; VkDeviceMemory imem; VkBuffer buf; VkDeviceMemory bmem; void *map;
    uint64_t frame, serial, keys[2 * RTD_KEYS]; uint32_t pass, att, w, h, tw, th, vkf, draws, nk;
} rtd_t;
static int g_armed; static uint64_t g_from, g_to;   // the frames dumped: [g_from, g_to)
static uint64_t g_fr; static uint32_t g_pass, g_draws, g_nk; static uint64_t g_keys[2 * RTD_KEYS];
static _Atomic int g_total;

int vka_rtdump_on(void) {
    int want = VKA_ENV("DM_VK_RTDUMP", 0);
    if (want != g_armed) { g_armed = want; g_from = vka_frame() + 1; g_to = want > 0 ? g_from + (uint64_t)want : 0; atomic_store(&g_total, 0); }
    uint64_t f = vka_frame();
    if (f < g_from || f >= g_to) return 0;
    if (f != g_fr) { g_fr = f; g_pass = 0; g_draws = 0; g_nk = 0; }
    return 1;
}
void vka_rtdump_draw(uint64_t vs, uint64_t fs) {   // a draw of the open pass: counted, its shader pair kept when it is among the pass's first distinct ones
    g_draws++;
    for (uint32_t i = 0; i < g_nk; i++) if (g_keys[2 * i] == vs && g_keys[2 * i + 1] == fs) return;
    if (g_nk < RTD_KEYS) { g_keys[2 * g_nk] = vs; g_keys[2 * g_nk + 1] = fs; g_nk++; }
}
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023u, u;
    if (e == 31) u = s | 0x7f800000u | (m << 13);
    else if (e) u = s | ((e + 112) << 23) | (m << 13);
    else if (!m) u = s;
    else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } u = s | (e << 23) | ((m & 1023u) << 13); }
    float f; memcpy(&f, &u, 4); return f;
}
static void rtd_free(rtd_t *d) {
    if (d->map) vk.vkUnmapMemory(V.dev, d->bmem);
    if (d->buf) vk.vkDestroyBuffer(V.dev, d->buf, NULL);
    if (d->bmem) vk.vkFreeMemory(V.dev, d->bmem, NULL);
    if (d->img) vk.vkDestroyImage(V.dev, d->img, NULL);
    if (d->imem) vk.vkFreeMemory(V.dev, d->imem, NULL);
    free(d);
}
static void rtd_write(void *arg) {   // the completion thread, once the submission that recorded it is done
    rtd_t *d = arg; const uint16_t *p = d->map; size_t n = (size_t)d->tw * d->th;
    double sum = 0; float mx = 0; size_t over1 = 0, over4 = 0, bad = 0;
    for (size_t i = 0; i < n; i++) {
        float r = half_f(p[4 * i]), g = half_f(p[4 * i + 1]), b = half_f(p[4 * i + 2]);
        if (!isfinite(r) || !isfinite(g) || !isfinite(b)) { bad++; continue; }
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * b; sum += l; if (l > mx) mx = l; over1 += l > 1.0f; over4 += l > 4.0f;
    }
    char fn[512], ks[160] = ""; size_t o = 0;
    for (uint32_t i = 0; i < d->nk && o < sizeof ks; i++) o += (size_t)snprintf(ks + o, sizeof ks - o, " %016llx/%016llx", (unsigned long long)d->keys[2 * i], (unsigned long long)d->keys[2 * i + 1]);
    snprintf(fn, sizeof fn, "%s/rt-f%llu-p%u-a%u-s%llu-%ux%u.rgba16f", getenv("DM_TEMP") ? getenv("DM_TEMP") : "/data/local/tmp", (unsigned long long)d->frame, d->pass, d->att,
             (unsigned long long)d->serial, d->tw, d->th);
    FILE *f = fopen(fn, "wb"); if (f) { fwrite(d->map, 8, n, f); fclose(f); }
    VLOG("rt dump: frame %llu pass %u attachment %u serial %llu %ux%u VkFormat %u, %u draws%s: mean %.3f, largest %.3g, above 1 %.1f %%, above 4 %.1f %%, NaN / infinite %zu%s",
         (unsigned long long)d->frame, d->pass, d->att, (unsigned long long)d->serial, d->w, d->h, d->vkf, d->draws, ks[0] ? ks : " (no draw)", n > bad ? sum / (double)(n - bad) : 0.0,
         mx, n ? 100.0 * (double)over1 / (double)n : 0.0, n ? 100.0 * (double)over4 / (double)n : 0.0, bad, f ? "" : " (thumbnail not written)");
    rtd_free(d);
}
static int blit_src_ok(VkFormat fmt) {   // (format properties asked once per format)
    static VkFormat seen[64]; static int ok[64], ns;
    for (int i = 0; i < ns; i++) if (seen[i] == fmt) return ok[i];
    VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, fmt, &fp);
    int r = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ? ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 2 : 1) : 0;
    if (ns < 64) { seen[ns] = fmt; ok[ns] = r; ns++; }
    return r;
}
// After a render pass ends (vka_draw.c vka_rp_end): its colour targets at the level and first layer the pass drew
void vka_rtdump_pass(VkCommandBuffer cb, vka_img *const *ims, const uint32_t *lvs, const uint32_t *l0s, int n) {
    uint32_t pass = g_pass++, draws = g_draws, nk = g_nk; uint64_t keys[2 * RTD_KEYS]; memcpy(keys, g_keys, sizeof keys);
    g_draws = 0; g_nk = 0;
    int bar = 0;
    for (int i = 0; i < n; i++) {
        vka_img *im = ims[i];
        if (!im || im->samples != 1 || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->f.kind != 0 || im->d.target == NVN_TEXTURE_TARGET_3D) continue;
        int filt = blit_src_ok((VkFormat)im->f.vk); if (!filt) continue;
        if (atomic_fetch_add(&g_total, 1) >= RTD_MAX) return;
        uint32_t w = im->w >> lvs[i], h = im->h >> lvs[i]; if (!w) w = 1; if (!h) h = 1;
        uint32_t tw = w < RTD_W ? w : RTD_W, th = (uint32_t)((uint64_t)h * tw / w); if (!th) th = 1;
        rtd_t *d = calloc(1, sizeof *d); if (!d) continue;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {tw, th, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)tw * th * 8, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkMemoryRequirements mi, mb;
        if (vk.vkCreateImage(V.dev, &ii, NULL, &d->img)) { rtd_free(d); continue; }
        vk.vkGetImageMemoryRequirements(V.dev, d->img, &mi);
        uint32_t ti = vka_mem_type(mi.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mi.size, .memoryTypeIndex = ti};
        if (ti == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &d->imem) || vk.vkBindImageMemory(V.dev, d->img, d->imem, 0) || vk.vkCreateBuffer(V.dev, &bi, NULL, &d->buf)) { rtd_free(d); continue; }
        vk.vkGetBufferMemoryRequirements(V.dev, d->buf, &mb);
        uint32_t tb = vka_mem_type(mb.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        VkMemoryAllocateInfo ab = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mb.size, .memoryTypeIndex = tb};
        if (tb == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ab, NULL, &d->bmem) || vk.vkBindBufferMemory(V.dev, d->buf, d->bmem, 0) || vk.vkMapMemory(V.dev, d->bmem, 0, VK_WHOLE_SIZE, 0, &d->map)) {
            d->map = NULL; rtd_free(d); continue; }
        if (!bar) { vka_barrier(cb); bar = 1; }   // the pass's writes before the blit reads them
        VkImageMemoryBarrier to = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = d->img,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to);
        VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, lvs[i], l0s[i], 1}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)tw, (int32_t)th, 1}}};
        vk.vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r, filt == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        VkMemoryBarrier tt = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {tw, th, 1}};
        vk.vkCmdCopyImageToBuffer(cb, d->img, VK_IMAGE_LAYOUT_GENERAL, d->buf, 1, &c);
        d->frame = vka_frame(); d->serial = im->serial; d->pass = pass; d->att = (uint32_t)i; d->w = w; d->h = h; d->tw = tw; d->th = th; d->vkf = (uint32_t)im->f.vk;
        d->draws = draws; d->nk = nk; memcpy(d->keys, keys, sizeof keys);
        vka_after(vka_fence_last() + 1, rtd_write, d);
    }
    if (bar) {
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        vka_barrier(cb);   // what comes after sees the targets as before
    }
}
