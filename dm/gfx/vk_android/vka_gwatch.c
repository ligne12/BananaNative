// CPU writes into memory the GPU is still using (DM_VK_GPU_WRITE_WATCH, read once): the storage buffers a recorded draw or dispatch may store into are write-protected for the
// CPU (nvn_dirty_protect, the texture tracker's own protection) from their recording until their submission is done. A guest thread that writes such a page before then is
// seen by a fault observer (crash.c dm_fault_observer_register, ahead of the tracker's handler, which then unprotects the page as for any write): 1 logs it (the address, the
// buffer, the shader that bound it, the writing instruction, how far behind the GPU is), 2 also makes the writer wait there until that submission is done, as the Switch 2's
// GPU would long have been; 3 waits for every submission reserved before the write (2 only for the one that bound the buffer: the next frame's had often started,
// and the game's reset of its exposure sums then landed in it: the whole picture flickered). Ocarina of Time on the AYN Thor (56 ms GPU frames) makes about 170 such writes a frame (it resets counters its culling adds into while the GPU
// still runs the frame before) with nothing to see: the foliage that dropped for one frame, first put down to them, was a translation fault (docs/reference/shaders.md)
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

enum { GWW_N = 8192, GWEV_N = 256 };
typedef struct { _Atomic uint64_t lo, hi, fence; uint64_t key; int st, kind; uint32_t aux; } gww_t;   // fence 0: its submission not committed yet; hi 0: being written
enum { GWW_STORE, GWW_UBO, GWW_SSBO_READ, GWW_PTR };   // what the entry watches: a storage buffer bound for stores; with DM_VK_GWW_READ a uniform buffer, a read-only storage
static const char *const g_kind[] = {"stores", "uniform buffer", "storage read", "page a uniform buffer points at"};   // buffer, the page a uniform buffer's word points at
static gww_t g_gww[GWW_N];
static _Atomic uint32_t g_gww_n; static uint32_t g_gww_from;   // entries made; the first one not committed (replay thread)
typedef struct { uint64_t addr, pc, lr, lo, hi, key, fence, done, wait_ns; int st, waited, kind; uint32_t aux; } gwev_t;
static gwev_t g_ev[GWEV_N]; static _Atomic uint64_t g_ev_w; static uint64_t g_ev_r;
static int g_mode = -1;
static _Atomic uint64_t g_hits, g_waits, g_timeouts;

static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// Signal handler context: atomics, clock_gettime and nanosleep only
static void observe(void *addr, int is_write, void *uc_) {
    if (!is_write) return;
    uint64_t a = (uint64_t)(uintptr_t)addr, done = vka_done(); uint32_t n = atomic_load(&g_gww_n);
    for (uint32_t k = 0; k < GWW_N && k < n; k++) {
        gww_t *e = &g_gww[(n - 1 - k) % GWW_N];
        uint64_t hi = atomic_load(&e->hi), lo = atomic_load(&e->lo), f = atomic_load(&e->fence);
        if (!hi || a < lo || a >= hi || (f && f <= done)) continue;
        uint64_t t0 = mono_ns(); int waited = 0, all = g_mode >= 3; uint64_t last = all ? vka_fence_last() : 0;
        if (g_mode >= 2) {   // 2: until the submission that bound it is committed and done; 3: until every submission reserved before this write is (a coherent memory's
                             // order: the write comes after all of them, before any later one); 500 ms at most
            for (;;) {
                f = atomic_load(&e->fence);
                if (all ? vka_done() >= last : (f && vka_done() >= f)) { waited = 1; break; }
                if (mono_ns() - t0 > 500000000ull) { atomic_fetch_add(&g_timeouts, 1); break; }
                struct timespec ts = {0, 50000}; nanosleep(&ts, NULL);
            }
            atomic_fetch_add(&g_waits, 1);
        }
        atomic_fetch_add(&g_hits, 1);
        uint64_t w = atomic_fetch_add(&g_ev_w, 1);
        const ucontext_t *uc = uc_;
        g_ev[w % GWEV_N] = (gwev_t){a, uc ? (uint64_t)uc->uc_mcontext.pc : 0, uc ? (uint64_t)uc->uc_mcontext.regs[30] : 0, lo, hi, e->key, f, done, mono_ns() - t0, e->st, waited, e->kind, e->aux};
        return;
    }
}

// DM_VK_GWW_READ (read once, with DM_VK_GPU_WRITE_WATCH=1): the memory that compute, task and mesh programs read is watched instead of the storage buffers they store
// into: their uniform buffers, the pages a word of those buffers points at (an address in placed memory: the raw pointers a GPU culling pass follows to its records),
// and the read-only storage buffers of the keys listed. "all" (every such program) or a list of keys: a CPU write there before the reading submission is done is a
// write the console's GPU would have read before it (a race the frame in flight loses)
static int g_read = -1; static uint64_t g_rkeys[16]; static int g_nrk;
static void read_init(void) {
    const char *e = getenv("DM_VK_GWW_READ"); g_read = e && *e;
    if (g_read && strcmp(e, "all")) for (; *e && g_nrk < 16;) { char *x; uint64_t k = strtoull(e, &x, 16); if (x == e) break; g_rkeys[g_nrk++] = k; e = *x == ',' ? x + 1 : x; }
}
static void add(uint64_t lo, uint64_t n, uint64_t key, int st, int kind, uint32_t aux) {
    if (g_mode < 0) { g_mode = VKA_ENV("DM_VK_GPU_WRITE_WATCH", 0); read_init(); if (g_mode > 0) { dm_fault_observer_register(observe); VLOG("GPU write watch (DM_VK_GPU_WRITE_WATCH=%d): CPU writes into %s the GPU still uses are %s", g_mode, g_read ? "memory compute, task and mesh programs read (DM_VK_GWW_READ)" : "storage buffers", g_mode >= 2 ? "made to wait for it" : "logged"); } }
    if (g_mode <= 0 || !n) return;
    uint32_t c = atomic_load(&g_gww_n);
    for (uint32_t k = 0; k < 32 && k < c - g_gww_from && k < c; k++) {   // the same range again in this submission (bound draw after draw): one entry
        gww_t *e = &g_gww[(c - 1 - k) % GWW_N];
        if (!atomic_load(&e->fence) && atomic_load(&e->lo) == lo && atomic_load(&e->hi) == lo + n) return;
    }
    gww_t *e = &g_gww[c % GWW_N];
    atomic_store(&e->hi, 0); atomic_store(&e->lo, lo); e->key = key; e->st = st; e->kind = kind; e->aux = aux; atomic_store(&e->fence, 0); atomic_store(&e->hi, lo + n);
    atomic_store(&g_gww_n, c + 1);
    nvn_dirty_protect((const void *)(uintptr_t)lo, (size_t)n);
}
// The replay thread: a storage buffer bound for stores by a draw or dispatch being recorded
void vka_gww_add(uint64_t lo, uint64_t n, uint64_t key, int st) { if (g_read < 0) read_init(); if (!g_read) add(lo, n, key, st, GWW_STORE, 0); }
// The replay thread: a buffer a compute, task or mesh stage reads (DM_VK_GWW_READ). A uniform buffer's words are looked at for addresses in placed memory
void vka_gww_read(uint64_t lo, uint64_t n, uint64_t key, int st, int storage, uint32_t binding) {
    if (g_read < 0) read_init();
    if (!g_read || st < 5 || !n) return;
    if (storage) { int hit = 0; for (int i = 0; i < g_nrk; i++) hit |= g_rkeys[i] == key; if (hit) add(lo, n < (2u << 20) ? n : (2u << 20), key, st, GWW_SSBO_READ, binding); return; }
    if (g_nrk) { int hit = 0; for (int i = 0; i < g_nrk; i++) hit |= g_rkeys[i] == key; if (!hit) return; }
    add(lo, n, key, st, GWW_UBO, binding);
    uint64_t pages[64]; int np = 0;
    const uint64_t *w = (const uint64_t *)(uintptr_t)lo;
    for (uint64_t i = 0; i < (n < 16384 ? n : 16384) / 8 && np < 64; i++) {
        uint64_t v = w[i], pg = v & ~4095ull; VkBuffer b; VkDeviceSize o;
        if (v < 0x10000000ull || (v & 3) || !vka_addr(v, 1, &b, &o, NULL)) continue;
        int seen = 0; for (int k = 0; k < np; k++) seen |= pages[k] == pg;
        if (!seen) { pages[np++] = pg; add(pg, 4096, key, st, GWW_PTR, binding << 16 | (uint32_t)(i * 8)); }
    }
}

// The replay thread: the entries since the last commit belong to this submission; and the writes seen since, logged (the first 64, then one in 64)
void vka_gww_commit(uint64_t fence) {
    if (g_mode <= 0) return;
    uint32_t c = atomic_load(&g_gww_n);
    for (uint32_t i = g_gww_from; i != c; i++) if (!atomic_load(&g_gww[i % GWW_N].fence)) atomic_store(&g_gww[i % GWW_N].fence, fence);
    g_gww_from = c;
    uint64_t w = atomic_load(&g_ev_w);
    if (w - g_ev_r > GWEV_N) g_ev_r = w - GWEV_N;
    for (; g_ev_r < w; g_ev_r++) {
        if (g_ev_r >= (g_read ? 4000u : 64u) && g_ev_r % 64) continue;
        const gwev_t *v = &g_ev[g_ev_r % GWEV_N];
        module_t *m = module_of((void *)(uintptr_t)v->pc), *ml = module_of((void *)(uintptr_t)v->lr);
        VLOG("GPU write watch %llu: CPU write at %#llx (+%#llx in %#llx-%#llx, %s of %016llx stage %d binding %u word %#x), submission fence %llu, GPU done %llu; pc %s+%#llx lr %s+%#llx%s %.2f ms",
             (unsigned long long)g_ev_r, (unsigned long long)v->addr, (unsigned long long)(v->addr - v->lo), (unsigned long long)v->lo, (unsigned long long)v->hi, g_kind[v->kind], (unsigned long long)v->key, v->st,
             v->kind == GWW_PTR ? v->aux >> 16 : v->aux, v->kind == GWW_PTR ? v->aux & 0xffff : 0,
             (unsigned long long)v->fence, (unsigned long long)v->done, m ? m->name : "?", (unsigned long long)(m ? v->pc - (uint64_t)m->base : v->pc), ml ? ml->name : "?",
             (unsigned long long)(ml ? v->lr - (uint64_t)ml->base : v->lr), v->waited ? ", waited" : "", v->wait_ns / 1e6);
    }
    static uint64_t subs; if (++subs % 1200 == 0)
        VLOG("GPU write watch: %llu CPU writes into buffers the GPU still used, %llu waits (%llu over 500 ms)", (unsigned long long)atomic_load(&g_hits), (unsigned long long)atomic_load(&g_waits),
             (unsigned long long)atomic_load(&g_timeouts));
}
#endif
