/* Host test for src/drivers/i915/gpu.c - the parts that can be checked
 * without the GPU:
 *   - the per-frame batch: every command's length field must match what
 *     was written (a miscount makes the engine run garbage and hang), the
 *     batch must end in MI_BATCH_BUFFER_END and fit its 4 KiB slot;
 *   - the golden state batch with its relocations applied;
 *   - the RECTLIST texture coordinates: the rasteriser's interpolation is
 *     simulated at every pixel centre for each rotation and must sample
 *     exactly the canvas pixel the CPU rotation in shell.c would copy.
 * Build/run: make check */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../src/drivers/i915/gpu.c"

/* ---- the kernel environment gpu.c expects ----------------------------------------- */
kernel_t k;
u64 k_now_us(void) { return 0; }
void hal_delay_us(u32 us) { (void)us; }
u32 hal_setting_get(const c16 *n, u32 def) { (void)n; return def; }
void hal_setting_set(const c16 *n, u32 v) { (void)n; (void)v; }
void *hal_dma_alloc(usize n) { return calloc(1, n); }
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void klog(const char *f, ...) { (void)f; }
void mm_uncached(u64 base, u64 size) { (void)base; (void)size; }
u64 mm_max_phys(void) { return 0; }
u32 pci_read32(u8 b, u8 d, u8 f, u16 o) { return 0; }
void pci_write32(u8 b, u8 d, u8 f, u16 o, u32 v) { }
u16 pci_read16(u8 b, u8 d, u8 f, u16 o) { return 0; }
u64 pci_bar(u8 b, u8 d, u8 f, int bar) { return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* walk a batch: returns the dword count up to and including MI_BATCH_BUFFER_END, or -1 */
static int walk(const u32 *b, int max, int *cmds) {
    int i = 0;
    *cmds = 0;
    while (i < max) {
        u32 h = b[i];
        int len;
        if (h == MI_NOOP) len = 1;
        else if (h == MI_BATCH_BUFFER_END) return i + 1;
        else if ((h >> 29) == 3 && (h & 0xffff0000u) == (CMD_PIPELINE_SELECT & 0xffff0000u)) len = 1;
        else if ((h >> 29) == 3) len = (int)(h & 0xff) + 2;
        else if ((h >> 29) == 0) len = (int)(h & 0x3f) + 2;          /* MI with a length field */
        else { printf("  unknown header %08x at %d\n", h, i); return -1; }
        (*cmds)++;
        i += len;
    }
    return -1;
}

static void test_batch(void) {
    g.arena = aligned_alloc(4096, ARENA_SIZE);
    memset(g.arena, 0, ARENA_SIZE);
    g.arena_gtt = 0x3ff00000;
    static_state();
    for (int n = 1; n <= VB_MAX; n += VB_MAX - 1) {
        memset(g.arena + A_BATCH, 0xee, 0x1000);
        bp = (u32 *)(g.arena + A_BATCH);
        emit_states(1200, 1920, n);
        int used = (int)(bp - (u32 *)(g.arena + A_BATCH));
        int cmds, end = walk((u32 *)(g.arena + A_BATCH), used, &cmds);
        CHECK(end > 0, "batch for %d rects does not end in MI_BATCH_BUFFER_END", n);
        CHECK(end == used || (end == used - 1 && ((u32 *)(g.arena + A_BATCH))[used - 1] == MI_NOOP),
              "batch walk ended at %d of %d dwords", end, used);
        CHECK(used * 4 <= 0x1000, "batch is %d bytes", used * 4);
        CHECK((used & 1) == 0, "batch length %d is not a whole qword", used);
        if (n == 1) printf("  frame batch: %d commands, %d dwords\n", cmds, used);
    }
    /* the state blocks must sit where the pointers say, aligned as required */
    CHECK((D_SAMPLER & 31) == 0 && (D_VIEWPORT & 31) == 0 && (D_CC & 63) == 0 && (D_BLEND & 63) == 0, "state alignment");
    CHECK((S_BTABLE & 31) == 0, "binding table alignment");
    CHECK(sizeof qrt_ps_kernel_gen8 <= 0x1000, "kernel too big");
    CHECK(VB_MAX * 48 <= 0x2000, "vertex buffer overflow");
    CHECK(A_ZERO + 0x1000 == ARENA_SIZE, "arena layout");

    /* the golden batch: walks to its end, relocations hit zero high dwords */
    int cmds, end = walk(qrt_golden_batch, (int)ARRAY_LEN(qrt_golden_batch), &cmds);
    CHECK(end > 0, "golden batch does not end");
    CHECK(sizeof qrt_golden_batch <= A_BATCH - A_GOLDEN, "golden batch is %zu bytes", sizeof qrt_golden_batch);
    for (usize i = 0; i < ARRAY_LEN(qrt_golden_relocs); i++)
        CHECK(qrt_golden_batch[qrt_golden_relocs[i] / 4 + 1] == 0, "reloc %zu high dword", i);
    printf("  golden batch: %d commands before the end, %zu bytes\n", cmds, sizeof qrt_golden_batch);
}

/* ---- rasterising a RECTLIST rectangle ---------------------------------------------- */
/* The attribute plane through the three vertices, evaluated at pixel centres,
 * as the setup engine and the affine shader do. */
static void plane(const float *v0, const float *v1, const float *v2, int a, double *A, double *B, double *C) {
    double x0 = v0[2], y0 = v0[3], x1 = v1[2], y1 = v1[3], x2 = v2[2], y2 = v2[3];
    double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    *B = ((v1[a] - v0[a]) * (y2 - y0) - (v2[a] - v0[a]) * (y1 - y0)) / det;
    *C = ((x1 - x0) * (v2[a] - v0[a]) - (x2 - x0) * (v1[a] - v0[a])) / det;
    *A = v0[a] - *B * x0 - *C * y0;
}

/* the CPU path (shell.c rotate_band): which canvas pixel lands on panel (px, py) */
static void cpu_source(int rot, int px, int py, int fw, int fh, int *lx, int *ly) {
    switch (rot) {
    case 0: *lx = px; *ly = py; break;
    case 1: *lx = py; *ly = fw - 1 - px; break;
    case 2: *lx = fw - 1 - px; *ly = fh - 1 - py; break;
    default: *lx = fh - 1 - py; *ly = px; break;
    }
}

static void test_coords(void) {
    const int fw = 1200, fh = 1920;
    int checked = 0;
    for (int rot = 0; rot < 4; rot++) {
        int sw = (rot & 1) ? fh : fw, sh = (rot & 1) ? fw : fh;          /* the logical canvas */
        const int rects[][4] = { { 0, 0, sw, sh }, { 13, 7, 101, 57 }, { sw - 33, sh - 21, 33, 21 }, { 0, sh - 1, sw, 1 } };
        for (usize ri = 0; ri < ARRAY_LEN(rects); ri++) {
            int x = rects[ri][0], y = rects[ri][1], w = rects[ri][2], h = rects[ri][3];
            grect_t r;
            switch (rot) {
            case 0: r = (grect_t){ x, y, w, h }; break;
            case 1: r = (grect_t){ fw - (y + h), x, h, w }; break;
            case 2: r = (grect_t){ fw - (x + w), fh - (y + h), w, h }; break;
            default: r = (grect_t){ y, fh - (x + w), h, w }; break;
            }
            float v[3][4];
            float x1 = r.x, y1 = r.y, x2 = r.x + r.w, y2 = r.y + r.h;
            vertex(v[0], rot, x2, y2, sw, sh, fw, fh);
            vertex(v[1], rot, x1, y2, sw, sh, fw, fh);
            vertex(v[2], rot, x1, y1, sw, sh, fw, fh);
            double ua, ub, uc, va, vb, vc;
            plane(v[0], v[1], v[2], 0, &ua, &ub, &uc);
            plane(v[0], v[1], v[2], 1, &va, &vb, &vc);
            int bad = 0;
            for (int py = r.y; py < r.y + r.h; py += (r.h > 64 ? 7 : 1))
                for (int px = r.x; px < r.x + r.w; px += (r.w > 64 ? 5 : 1)) {
                    float u = (float)(ua + ub * (px + 0.5) + uc * (py + 0.5));
                    float vv = (float)(va + vb * (px + 0.5) + vc * (py + 0.5));
                    int tx = (int)floorf(u * sw), ty = (int)floorf(vv * sh);
                    int lx, ly;
                    cpu_source(rot, px, py, fw, fh, &lx, &ly);
                    if (tx != lx || ty != ly) {
                        if (!bad++) printf("  rot %d rect %zu: panel (%d,%d) samples (%d,%d), CPU copies (%d,%d)\n",
                                           rot, ri, px, py, tx, ty, lx, ly);
                    }
                    /* the canvas pixel must also lie inside the damaged logical rectangle */
                    if (lx < x || lx >= x + w || ly < y || ly >= y + h) bad++;
                    checked++;
                }
            CHECK(!bad, "rotation %d, rectangle %zu: %d pixels sample the wrong texel", rot, ri, bad);
        }
    }
    printf("  texture coordinates: %d pixel centres checked\n", checked);
}

int main(void) {
    test_batch();
    test_coords();
    if (fails) { printf("test_gpu: %d failures\n", fails); return 1; }
    printf("test_gpu: ok\n");
    return 0;
}
