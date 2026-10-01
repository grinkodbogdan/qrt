/*
 * gpu.c - the 3D engine of Intel's Gen8 graphics (Cherry Trail "HD Graphics",
 * 8086:22b0..22b3), used to put the shell's picture on the panel.
 *
 * The shell draws a logical canvas (landscape on the Venue's portrait panel)
 * and used to turn it into the framebuffer with the CPU.  Here the GPU does
 * that copy: the canvas is a texture, the framebuffer the render target, and
 * each damaged rectangle is one RECTLIST whose texture coordinates are turned
 * by the screen rotation.  The CPU only waits for the result.
 *
 * Nothing in QRT sets the GPU up but this file, so it does what Linux's i915
 * does for a legacy (non-execlist) render ring on Cherryview, and no more:
 *
 *   forcewake       keep the render power well awake (FORCEWAKE_VLV)
 *   GGTT            our buffers and the canvas pages are entered at the top
 *                   of the global GTT; the framebuffer is already mapped by
 *                   the firmware - its GTT offset is where it sits in the
 *                   aperture (BAR 2)
 *   PPAT            entry 0 (the one GGTT accesses use) set to snoop, as
 *                   chv_setup_private_ppat() does
 *   workarounds     the gen8/chv render and clock-gating ones from i915
 *   ring            a 16 KiB legacy ring on RCS, a status page for seqnos
 *   golden state    i915's null render state batch, run once
 *
 * The per-frame batch is the one intel-vaapi-driver's gen8_render.c builds
 * to put a picture on screen (gen8_render_put_surface), with its shader.
 *
 * It starts only in native mode, runs a self-test on a small off-screen
 * surface and checks every pixel; any failure, and any frame that does not
 * finish within 100 ms, turns it off for good and the CPU path takes over.
 * A start that never returned (the tablet froze) is remembered in NVRAM and
 * the next boot leaves the GPU alone.
 */
#include "../../kernel/kernel.h"
#include "gpu.h"

#if defined(__x86_64__)
#include "../../arch/x64/mm.h"
#include "gen8_render.h"
#include "shaders_gen8.h"
#include "renderstate_gen8.h"

_Static_assert(sizeof(struct gen8_surface_state) == 64, "surface state");
_Static_assert(sizeof(struct gen8_sampler_state) == 16, "sampler state");

enum { G_NONE, G_OFF, G_READY, G_FAILED };

/* our buffers: one contiguous block, mapped at the top of the GGTT */
#define A_RING      0x0000
#define RING_SIZE   0x4000
#define A_HWS       0x4000          /* status page: seqno at +0x40, scratch at +0x80 */
#define A_GOLDEN    0x5000
#define A_BATCH     0x6000
#define A_SURF      0x7000          /* surface states 64 bytes apart, binding table at +0x100 */
#define A_DYN       0x8000          /* push constants, sampler, viewport, colour calc, blend */
#define A_KERNEL    0x9000
#define A_VB        0xa000          /* 8 KiB of vertices: 170 rectangles */
#define A_TSRC      0xc000          /* self-test source, 64x64 */
#define A_TDST      0x10000         /* self-test target */
#define A_ZERO      0x14000         /* mapped after the canvas against sampler over-fetch */
#define ARENA_SIZE  0x15000

#define D_CURBE     0x00
#define D_SAMPLER   0x40
#define D_VIEWPORT  0x80
#define D_CC        0xc0
#define D_BLEND     0x100
#define S_BTABLE    0x100

#define SEQ_OFF     0x40
#define SCRATCH_OFF 0x80
#define TEST_N      64
#define VB_MAX      ((0x2000 / 48))

#define SCENE_WINDOW (32u << 20)    /* GGTT space reserved for canvases: two slots */
#define SLOT_SIZE    (SCENE_WINDOW / 2)

static struct {
    int state;
    char status[112];
    pci_dev_t *pci;
    volatile u8 *mmio;
    volatile u64 *gsm;             /* GGTT entries */
    u64 gmadr;
    u64 ggtt_size;                 /* bytes of address space the GGTT covers */
    u32 fb_off;                    /* the framebuffer's GGTT offset */
    u8 *arena;
    u32 arena_gtt, scene_gtt;
    struct { const u8 *page; usize pages; u32 used; } slot[2];   /* canvases entered in the GGTT */
    u32 clock;
    u32 tail, seqno;
    int clflush;                   /* GPU reads are not snooped: write CPU caches back first */
    int guard;                     /* NVRAM crash guard still armed */
    u32 frames;
    u64 busy_us;
} g;

/* ---- low level ------------------------------------------------------------------ */
static u32 rd(u32 r) { return *(volatile u32 *)(g.mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(g.mmio + r) = v; }

static int wait_reg(u32 r, u32 mask, u32 want, u32 ms) {
    u64 end = k_now_us() + (u64)ms * 1000;
    while ((rd(r) & mask) != want)
        if (k_now_us() > end) return 0;
    return 1;
}

static void flush_range(const void *p, usize n) {
    usize a = (usize)p & ~(usize)63, e = (usize)p + n;
    __asm__ volatile("mfence" ::: "memory");
    for (; a < e; a += 64) __asm__ volatile("clflush (%0)" : : "r"(a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static void gtt_map(u32 off, u64 phys, usize pages) {
    volatile u64 *pte = g.gsm + (off >> 12);
    for (usize i = 0; i < pages; i++) pte[i] = (phys + i * 4096) | 3;     /* present | writable */
    (void)pte[pages - 1];
    wr(GFX_FLSH_CNTL_GEN6, 1);
    (void)rd(GFX_FLSH_CNTL_GEN6);
}

static int forcewake(void) {
    wait_reg(FORCEWAKE_ACK_VLV, FORCEWAKE_KERNEL, 0, 50);
    wr(FORCEWAKE_VLV, MASKED_ENABLE(FORCEWAKE_KERNEL));
    if (wait_reg(FORCEWAKE_ACK_VLV, FORCEWAKE_KERNEL, FORCEWAKE_KERNEL, 50)) return 1;
    /* the GT may not be allowed to wake (vlv_allow_gt_wake) */
    wr(VLV_GTLC_WAKE_CTRL, rd(VLV_GTLC_WAKE_CTRL) | VLV_GTLC_ALLOWWAKEREQ);
    wait_reg(VLV_GTLC_PW_STATUS, 1, 1, 50);
    wr(FORCEWAKE_VLV, MASKED_ENABLE(FORCEWAKE_KERNEL));
    return wait_reg(FORCEWAKE_ACK_VLV, FORCEWAKE_KERNEL, FORCEWAKE_KERNEL, 50);
}

static void fail(const char *why) {
    g.state = G_FAILED;
    fmt(g.status, sizeof g.status, "GPU off: %s", why);
    klog("gpu: %s", g.status);
    if (g.guard) { hal_setting_set(u"QrtGpuGuard", 0); g.guard = 0; }   /* handled: not a freeze */
}

static void hang(const char *what) {
    char b[112];
    fmt(b, sizeof b, "%s did not finish (head %x tail %x acthd %x ipehr %x eir %x err %x)", what,
        rd(RCS_HEAD), rd(RCS_TAIL), rd(RCS_ACTHD), rd(RCS_IPEHR), rd(EIR), rd(ERROR_GEN6));
    fail(b);
    wr(GEN6_GDRST, GEN6_GRDOM_FULL);                  /* reset the GT; the display is not part of it */
    wait_reg(GEN6_GDRST, GEN6_GRDOM_FULL, 0, 50);
}

/* ---- the ring ------------------------------------------------------------------- */
static u32 *ring(void) { return (u32 *)(g.arena + A_RING); }

static void ring_begin(u32 dwords) {
    if (g.tail + dwords * 4 > RING_SIZE - 8) {        /* the engine is idle: just wrap */
        while (g.tail < RING_SIZE) { ring()[g.tail / 4] = MI_NOOP; g.tail += 4; }
        g.tail = 0;
    }
}
static void ring_out(u32 v) { ring()[g.tail / 4] = v; g.tail += 4; }

static void pipe_control(u32 flags, u32 addr, u32 data) {
    ring_out(GFX_OP_PIPE_CONTROL(6));
    ring_out(flags);
    ring_out(addr);
    ring_out(0);
    ring_out(data);
    ring_out(0);
}

/* Run a batch and wait for it.  Returns 0 (and turns the GPU off) on a hang. */
static int run_batch(u32 batch_gtt, const char *what) {
    u32 start = g.tail;
    ring_begin(32);
    if (g.tail < start) { flush_range(ring() + start / 4, RING_SIZE - start); start = 0; }
    u32 scratch = g.arena_gtt + A_HWS + SCRATCH_OFF;
    /* WaCsStallBeforeStateCacheInvalidate, then invalidate what the CPU may have changed */
    pipe_control(PIPE_CONTROL_CS_STALL | PIPE_CONTROL_STALL_AT_SCOREBOARD, 0, 0);
    pipe_control(PIPE_CONTROL_CS_STALL | PIPE_CONTROL_TLB_INVALIDATE | PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
                 PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE | PIPE_CONTROL_VF_CACHE_INVALIDATE |
                 PIPE_CONTROL_CONST_CACHE_INVALIDATE | PIPE_CONTROL_STATE_CACHE_INVALIDATE |
                 PIPE_CONTROL_QW_WRITE | PIPE_CONTROL_GLOBAL_GTT_IVB, scratch, 0);
    ring_out(MI_BATCH_BUFFER_START_GEN8);              /* bit 8 clear: the batch is in the GGTT */
    ring_out(batch_gtt);
    ring_out(0);
    pipe_control(PIPE_CONTROL_CS_STALL | PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH |
                 PIPE_CONTROL_DC_FLUSH_ENABLE | PIPE_CONTROL_FLUSH_ENABLE, scratch, 0);
    u32 seq = ++g.seqno;
    pipe_control(PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_CS_STALL | PIPE_CONTROL_QW_WRITE,
                 g.arena_gtt + A_HWS + SEQ_OFF, seq);
    ring_out(MI_NOOP);                                 /* the tail stays 8-byte aligned */
    flush_range(ring() + start / 4, g.tail - start);
    wr(RCS_TAIL, g.tail);

    volatile u32 *sp = (volatile u32 *)(g.arena + A_HWS + SEQ_OFF);
    u64 t0 = k_now_us(), end = t0 + 100000;
    for (;;) {
        flush_range((const void *)sp, 4);
        if ((int)(*sp - seq) >= 0) break;
        if (k_now_us() > end) { hang(what); return 0; }
        __asm__ volatile("pause");
    }
    g.busy_us += k_now_us() - t0;
    return 1;
}

/* ---- state -------------------------------------------------------------------- */
static void surface(int index, u32 gtt, int w, int h, int pitch, int format) {
    struct gen8_surface_state *ss = (struct gen8_surface_state *)(g.arena + A_SURF + index * 64);
    memset(ss, 0, sizeof *ss);
    ss->ss0.surface_type = I965_SURFACE_2D;
    ss->ss0.surface_format = format;
    ss->ss0.vertical_alignment = 1;                    /* "always 1 (align 4)" per the B-spec */
    ss->ss0.horizontal_alignment = 1;
    ss->ss8.base_addr = gtt;
    ss->ss2.width = w - 1;
    ss->ss2.height = h - 1;
    ss->ss3.pitch = pitch - 1;
    ss->ss7.shader_chanel_select_r = HSW_SCS_RED;
    ss->ss7.shader_chanel_select_g = HSW_SCS_GREEN;
    ss->ss7.shader_chanel_select_b = HSW_SCS_BLUE;
    ss->ss7.shader_chanel_select_a = HSW_SCS_ALPHA;
    ((u32 *)(g.arena + A_SURF + S_BTABLE))[index] = index * 64;
}

/* the fixed part: sampler, viewport, colour calc, blend, constants, shader */
static void static_state(void) {
    u8 *dyn = g.arena + A_DYN;
    *(float *)(dyn + D_CURBE) = 1.0f;                  /* global alpha for exa_wm_src_sample_argb */
    struct gen8_sampler_state *s = (struct gen8_sampler_state *)(dyn + D_SAMPLER);
    memset(s, 0, sizeof *s);
    s->ss0.min_filter = I965_MAPFILTER_NEAREST;        /* exact pixel copies */
    s->ss0.mag_filter = I965_MAPFILTER_NEAREST;
    s->ss3.r_wrap_mode = s->ss3.s_wrap_mode = s->ss3.t_wrap_mode = I965_TEXCOORDMODE_CLAMP;
    struct i965_cc_viewport *vp = (struct i965_cc_viewport *)(dyn + D_VIEWPORT);
    vp->min_depth = -1.e35f;
    vp->max_depth = 1.e35f;
    struct gen6_color_calc_state *cc = (struct gen6_color_calc_state *)(dyn + D_CC);
    memset(cc, 0, sizeof *cc);
    cc->constant_r = 1.0f; cc->constant_b = 1.0f; cc->constant_a = 1.0f;
    struct gen8_global_blend_state *gb = (struct gen8_global_blend_state *)(dyn + D_BLEND);
    memset(gb, 0, sizeof *gb + 16 * sizeof(struct gen8_blend_state_rt));
    struct gen8_blend_state_rt *rt = (struct gen8_blend_state_rt *)(gb + 1);
    rt->blend1.logic_op_enable = 1;                    /* COPY */
    rt->blend1.logic_op_func = 0xc;
    rt->blend1.pre_blend_clamp_enable = 1;
    memcpy(g.arena + A_KERNEL, qrt_ps_kernel_gen8, sizeof qrt_ps_kernel_gen8);
}

/* ---- the batch (gen8_render_emit_states, one draw) ----------------------------------- */
static u32 *bp;
#define OUT(v) (*bp++ = (u32)(v))

static void emit_states(int dst_w, int dst_h, int nrect) {
    u32 base = g.arena_gtt;
    /* intel_batchbuffer_emit_mi_flush */
    OUT(GFX_OP_PIPE_CONTROL(6));
    OUT(PIPE_CONTROL_CS_STALL | PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE |
        PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE | PIPE_CONTROL_DC_FLUSH_ENABLE);
    OUT(0); OUT(0); OUT(0); OUT(0);

    /* invariant states */
    OUT(CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D);
    OUT(GEN8_3DSTATE_MULTISAMPLE | 0);
    OUT(GEN6_3DSTATE_MULTISAMPLE_PIXEL_LOCATION_CENTER | GEN6_3DSTATE_MULTISAMPLE_NUMSAMPLES_1);
    OUT(GEN8_3DSTATE_SAMPLE_PATTERN | (9 - 2));
    for (int i = 0; i < 8; i++) OUT(0);
    OUT(GEN6_3DSTATE_SAMPLE_MASK | 0); OUT(1);
    OUT(CMD_STATE_SIP | 0); OUT(0); OUT(0);

    /* state base addresses */
    OUT(CMD_STATE_BASE_ADDRESS | (16 - 2));
    OUT(BASE_ADDRESS_MODIFY); OUT(0); OUT(0);
    OUT((base + A_SURF) | BASE_ADDRESS_MODIFY); OUT(0);
    OUT((base + A_DYN) | BASE_ADDRESS_MODIFY); OUT(0);
    OUT(BASE_ADDRESS_MODIFY); OUT(0);
    OUT((base + A_KERNEL) | BASE_ADDRESS_MODIFY); OUT(0);
    for (int i = 0; i < 4; i++) OUT(0xffff0000u | BASE_ADDRESS_MODIFY);

    /* viewports */
    OUT(GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC | 0); OUT(D_VIEWPORT);
    OUT(GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CL | 0); OUT(0);

    /* URB: 8 KiB of push constants for the PS, the rest for VS entries */
    OUT(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS | 0); OUT(0);
    OUT(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_DS | 0); OUT(0);
    OUT(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_HS | 0); OUT(0);
    OUT(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_GS | 0); OUT(0);
    OUT(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS | 0);
    OUT((0 << GEN8_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT) | (8 << GEN8_PUSH_CONSTANT_BUFFER_SIZE_SHIFT));
    OUT(GEN7_3DSTATE_URB_VS | 0);
    OUT((64 << GEN7_URB_ENTRY_NUMBER_SHIFT) | ((4 - 1) << GEN7_URB_ENTRY_SIZE_SHIFT) | (4 << GEN7_URB_STARTING_ADDRESS_SHIFT));
    OUT(GEN7_3DSTATE_URB_GS | 0); OUT(5 << GEN7_URB_STARTING_ADDRESS_SHIFT);
    OUT(GEN7_3DSTATE_URB_HS | 0); OUT(6 << GEN7_URB_STARTING_ADDRESS_SHIFT);
    OUT(GEN7_3DSTATE_URB_DS | 0); OUT(7 << GEN7_URB_STARTING_ADDRESS_SHIFT);

    /* colour calc / blend / sampler pointers */
    OUT(GEN6_3DSTATE_CC_STATE_POINTERS | 0); OUT(D_CC + 1);
    OUT(GEN7_3DSTATE_BLEND_STATE_POINTERS | 0); OUT(D_BLEND + 1);
    OUT(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS | 0); OUT(D_SAMPLER);

    OUT(GEN8_3DSTATE_WM_HZ_OP | (5 - 2)); OUT(0); OUT(0); OUT(0); OUT(0);

    /* GS, HS, TE, DS, stream-out: off */
    OUT(GEN6_3DSTATE_CONSTANT_GS | (11 - 2)); for (int i = 0; i < 10; i++) OUT(0);
    OUT(GEN6_3DSTATE_GS | (10 - 2)); for (int i = 0; i < 9; i++) OUT(0);
    OUT(GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS | 0); OUT(0);
    OUT(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS | 0); OUT(0);
    OUT(GEN7_3DSTATE_CONSTANT_HS | (11 - 2)); for (int i = 0; i < 10; i++) OUT(0);
    OUT(GEN7_3DSTATE_HS | (9 - 2)); for (int i = 0; i < 8; i++) OUT(0);
    OUT(GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS | 0); OUT(0);
    OUT(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_HS | 0); OUT(0);
    OUT(GEN7_3DSTATE_TE | (4 - 2)); OUT(0); OUT(0); OUT(0);
    OUT(GEN7_3DSTATE_CONSTANT_DS | (11 - 2)); for (int i = 0; i < 10; i++) OUT(0);
    OUT(GEN7_3DSTATE_DS | (9 - 2)); for (int i = 0; i < 8; i++) OUT(0);
    OUT(GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS | 0); OUT(0);
    OUT(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_DS | 0); OUT(0);
    OUT(GEN7_3DSTATE_STREAMOUT | (5 - 2)); OUT(0); OUT(0); OUT(0); OUT(0);

    /* VS: pass-through */
    OUT(GEN6_3DSTATE_CONSTANT_VS | (11 - 2)); for (int i = 0; i < 10; i++) OUT(0);
    OUT(GEN6_3DSTATE_VS | (9 - 2)); for (int i = 0; i < 8; i++) OUT(0);
    OUT(GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS | 0); OUT(0);
    OUT(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS | 0); OUT(0);

    /* clip: pass-through */
    OUT(GEN6_3DSTATE_CLIP | (4 - 2)); OUT(0); OUT(0); OUT(0);

    /* SF / SBE: one attribute (the texture coordinate) */
    OUT(GEN8_3DSTATE_RASTER | (5 - 2)); OUT(GEN8_3DSTATE_RASTER_CULL_NONE); OUT(0); OUT(0); OUT(0);
    OUT(GEN7_3DSTATE_SBE | (4 - 2));
    OUT(GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH | GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET |
        (1 << GEN7_SBE_NUM_OUTPUTS_SHIFT) | (1 << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT) |
        (1 << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT));
    OUT(0); OUT(0);
    OUT(GEN8_3DSTATE_SBE_SWIZ | (11 - 2)); for (int i = 0; i < 10; i++) OUT(0);
    OUT(GEN6_3DSTATE_SF | (4 - 2)); OUT(0); OUT(0); OUT(2 << GEN6_3DSTATE_SF_TRIFAN_PROVOKE_SHIFT);

    /* no depth or stencil */
    OUT(GEN8_3DSTATE_WM_DEPTH_STENCIL | (3 - 2)); OUT(0); OUT(0);
    OUT(GEN7_3DSTATE_DEPTH_BUFFER | (8 - 2));
    OUT((I965_DEPTHFORMAT_D32_FLOAT << 18) | (I965_SURFACE_NULL << 29));
    for (int i = 0; i < 6; i++) OUT(0);
    OUT(GEN7_3DSTATE_HIER_DEPTH_BUFFER | (5 - 2)); OUT(0); OUT(0); OUT(0); OUT(0);
    OUT(GEN7_3DSTATE_STENCIL_BUFFER | (5 - 2)); OUT(0); OUT(0); OUT(0); OUT(0);
    OUT(GEN7_3DSTATE_CLEAR_PARAMS | (3 - 2)); OUT(0); OUT(0);

    OUT(CMD_DRAWING_RECTANGLE | 2);
    OUT(0);
    OUT((dst_w - 1) | (dst_h - 1) << 16);
    OUT(0);

    /* vertex elements: VUE header (zeros), position from offset 8, texcoord from 0 */
    OUT(CMD_VERTEX_ELEMENTS | (7 - 2));
    OUT((0 << GEN8_VE0_VERTEX_BUFFER_INDEX_SHIFT) | GEN8_VE0_VALID |
        (I965_SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT) | (0 << VE0_OFFSET_SHIFT));
    OUT((I965_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT) | (I965_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT) |
        (I965_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT) | (I965_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT));
    OUT((0 << GEN8_VE0_VERTEX_BUFFER_INDEX_SHIFT) | GEN8_VE0_VALID |
        (I965_SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT) | (8 << VE0_OFFSET_SHIFT));
    OUT((I965_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT) | (I965_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT) |
        (I965_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT) | (I965_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT));
    OUT((0 << GEN8_VE0_VERTEX_BUFFER_INDEX_SHIFT) | GEN8_VE0_VALID |
        (I965_SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT) | (0 << VE0_OFFSET_SHIFT));
    OUT((I965_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT) | (I965_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT) |
        (I965_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT) | (I965_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT));
    for (int i = 0; i < 3; i++) { OUT(GEN8_3DSTATE_VF_INSTANCING | (3 - 2)); OUT(i); OUT(0); }
    OUT(GEN8_3DSTATE_VF_SGVS | 0); OUT(0);

    /* pixel shader */
    OUT(GEN8_3DSTATE_PSEXTRA | 0); OUT(GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE);
    OUT(GEN8_3DSTATE_PSBLEND | 0); OUT(GEN8_PS_BLEND_HAS_WRITEABLE_RT);
    OUT(GEN6_3DSTATE_WM | 0); OUT(GEN7_WM_PERSPECTIVE_PIXEL_BARYCENTRIC);
    OUT(GEN6_3DSTATE_CONSTANT_PS | (11 - 2));
    OUT(4);                                            /* buffer 0: 4 registers */
    OUT(0);
    OUT(D_CURBE); OUT(0);                              /* relative to the dynamic state base */
    for (int i = 0; i < 6; i++) OUT(0);
    OUT(GEN7_3DSTATE_PS | (12 - 2));
    OUT(0); OUT(0);                                    /* kernel at the instruction base */
    OUT((1 << GEN7_PS_SAMPLER_COUNT_SHIFT) | (5 << GEN7_PS_BINDING_TABLE_ENTRY_COUNT_SHIFT) | GEN7_PS_VECTOR_MASK_ENABLE);
    OUT(0); OUT(0);
    OUT(((64 - 2 - 1) << GEN8_PS_MAX_THREADS_SHIFT) | GEN7_PS_PUSH_CONSTANT_ENABLE | GEN7_PS_16_DISPATCH_ENABLE);
    OUT(6 << GEN7_PS_DISPATCH_START_GRF_SHIFT_0);
    OUT(0); OUT(0); OUT(0); OUT(0);
    OUT(GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS | 0); OUT(S_BTABLE);

    /* the rectangles */
    OUT(CMD_VERTEX_BUFFERS | (5 - 2));
    OUT((0 << GEN8_VB0_BUFFER_INDEX_SHIFT) | (0 << GEN8_VB0_MOCS_SHIFT) | GEN7_VB0_ADDRESS_MODIFYENABLE |
        ((4 * 4) << VB0_BUFFER_PITCH_SHIFT));
    OUT(base + A_VB); OUT(0);
    OUT(nrect * 48);
    OUT(GEN8_3DSTATE_VF_TOPOLOGY | 0); OUT(_3DPRIM_RECTLIST);
    OUT(CMD_3DPRIMITIVE | (7 - 2));
    OUT(GEN7_3DPRIM_VERTEXBUFFER_ACCESS_SEQUENTIAL);
    OUT(3 * nrect);
    OUT(0); OUT(1); OUT(0); OUT(0);

    OUT(MI_BATCH_BUFFER_END);
    if (((usize)bp & 7)) OUT(MI_NOOP);
}

/* One rectangle of the destination with the texture coordinates of its
 * corners: RECTLIST takes bottom-right, bottom-left, top-left. */
typedef struct { int x, y, w, h; } grect_t;

static void tex(int rot, float X, float Y, int sw, int sh, int dw, int dh, float *u, float *v) {
    switch (rot & 3) {
    case 0: *u = X / sw; *v = Y / sh; break;
    case 1: *u = Y / sw; *v = (dw - X) / sh; break;       /* logical (lx, ly) -> panel (dw-1-ly, lx) */
    case 2: *u = (dw - X) / sw; *v = (dh - Y) / sh; break;
    default: *u = (dh - Y) / sw; *v = X / sh; break;      /* logical (lx, ly) -> panel (ly, dh-1-lx) */
    }
}

static void vertex(float *vb, int rot, float X, float Y, int sw, int sh, int dw, int dh) {
    tex(rot, X, Y, sw, sh, dw, dh, &vb[0], &vb[1]);
    vb[2] = X; vb[3] = Y;
}

/* draw: src surface (index 1) -> dst surface (index 0) */
static int draw(u32 src_gtt, int sw, int sh, int spitch, u32 dst_gtt, int dw, int dh, int dpitch, int dfmt,
                int rot, const grect_t *r, int n, const char *what) {
    surface(0, dst_gtt, dw, dh, dpitch, dfmt);
    surface(1, src_gtt, sw, sh, spitch, I965_SURFACEFORMAT_B8G8R8A8_UNORM);
    float *vb = (float *)(g.arena + A_VB);
    for (int i = 0; i < n; i++, vb += 12) {
        float x1 = r[i].x, y1 = r[i].y, x2 = r[i].x + r[i].w, y2 = r[i].y + r[i].h;
        vertex(vb + 0, rot, x2, y2, sw, sh, dw, dh);
        vertex(vb + 4, rot, x1, y2, sw, sh, dw, dh);
        vertex(vb + 8, rot, x1, y1, sw, sh, dw, dh);
    }
    bp = (u32 *)(g.arena + A_BATCH);
    emit_states(dw, dh, n);
    flush_range(g.arena + A_BATCH, (u8 *)bp - (g.arena + A_BATCH));
    flush_range(g.arena + A_SURF, 0x200);
    flush_range(g.arena + A_VB, n * 48);
    return run_batch(g.arena_gtt + A_BATCH, what);
}

/* ---- set-up -------------------------------------------------------------------- */
static void workarounds(void) {
    /* gen8_init_workarounds + chv_init_workarounds (written directly: no contexts here) */
    wr(RCS_INSTPM, MASKED_ENABLE(INSTPM_FORCE_ORDERING));
    wr(RCS_MI_MODE, MASKED_ENABLE(ASYNC_FLIP_PERF_DISABLE));
    wr(GEN8_ROW_CHICKEN, MASKED_ENABLE(PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE | STALL_DOP_GATING_DISABLE));
    wr(HDC_CHICKEN0, MASKED_ENABLE(HDC_DONOT_FETCH_MEM_WHEN_MASKED | HDC_FORCE_NON_COHERENT));
    wr(CACHE_MODE_0_GEN7, MASKED_DISABLE(HIZ_RAW_STALL_OPT_DISABLE));
    wr(CACHE_MODE_1, MASKED_ENABLE(GEN8_4x4_STC_OPTIMIZATION_DISABLE));
    wr(GEN7_GT_MODE, MASKED_FIELD(GEN6_WIZ_HASHING_MASK, GEN6_WIZ_HASHING_16x4));
    wr(HIZ_CHICKEN, MASKED_ENABLE(CHV_HZ_8X8_MODE_IN_1X));
    /* cherryview_init_clock_gating */
    wr(GEN7_FF_THREAD_MODE, rd(GEN7_FF_THREAD_MODE) & ~(GEN8_FF_DS_REF_CNT_FFME | GEN7_FF_VS_REF_CNT_FFME));
    wr(GEN6_RC_SLEEP_PSMI_CONTROL, MASKED_ENABLE(GEN8_RC_SEMA_IDLE_MSG_DISABLE));
    wr(GEN6_UCGCTL1, rd(GEN6_UCGCTL1) | GEN6_CSUNIT_CLOCK_GATE_DISABLE);
    wr(GEN8_UCGCTL6, rd(GEN8_UCGCTL6) | GEN8_SDEUNIT_CLOCK_GATE_DISABLE);
    u32 misc = rd(GEN7_MISCCPCTL);                     /* WaProgramL3SqcReg1Default:chv */
    wr(GEN7_MISCCPCTL, misc & ~GEN7_DOP_CLOCK_GATE_ENABLE);
    wr(GEN8_L3SQCREG1, ((38 >> 1) << 19) | ((2 >> 1) << 14));
    (void)rd(GEN8_L3SQCREG1);
    hal_delay_us(1);
    wr(GEN7_MISCCPCTL, misc);
    wr(HSW_GTT_CACHE_EN, GTT_CACHE_EN_ALL);
}

static int ring_init(void) {
    wr(GFX_MODE_GEN7, MASKED_DISABLE(GFX_RUN_LIST_ENABLE));   /* legacy ring, no execlists */
    /* stop_ring() */
    wr(RCS_MI_MODE, MASKED_ENABLE(STOP_RING));
    if (!wait_reg(RCS_MI_MODE, MODE_IDLE, MODE_IDLE, 1000) && rd(RCS_HEAD) != rd(RCS_TAIL)) return 0;
    wr(RCS_CTL, 0);
    wr(RCS_HEAD, 0);
    wr(RCS_TAIL, 0);
    (void)rd(RCS_CTL);
    wr(RCS_MI_MODE, MASKED_DISABLE(STOP_RING));
    if (rd(RCS_HEAD) & 0x001ffffc) return 0;
    /* status page, then the ring */
    wr(RCS_HWS_PGA, g.arena_gtt + A_HWS);
    (void)rd(RCS_HWS_PGA);
    (void)rd(RCS_HEAD);
    wr(RCS_START, g.arena_gtt + A_RING);
    wr(RCS_HEAD, 0);
    wr(RCS_TAIL, 0);
    (void)rd(RCS_TAIL);
    wr(RCS_CTL, ((RING_SIZE - 4096) & RING_NR_PAGES) | RING_VALID);
    if (!wait_reg(RCS_CTL, RING_VALID, RING_VALID, 50)) return 0;
    g.tail = 0;
    return 1;
}

/* the off-screen check: rotate a 64x64 pattern, read every pixel back */
static int self_test(void) {
    u32 *src = (u32 *)(g.arena + A_TSRC), *dst = (u32 *)(g.arena + A_TDST);
    for (int y = 0; y < TEST_N; y++)
        for (int x = 0; x < TEST_N; x++)
            src[y * TEST_N + x] = 0xff000000u | (u32)(x * 4) << 16 | (u32)(y * 4) << 8 | (u32)((x * 7 + y * 13) & 0xff);
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(dst, 0, TEST_N * TEST_N * 4);
        flush_range(dst, TEST_N * TEST_N * 4);
        if (g.clflush) flush_range(src, TEST_N * TEST_N * 4);
        grect_t r[2] = { { 0, 0, TEST_N, TEST_N / 2 }, { 0, TEST_N / 2, TEST_N, TEST_N / 2 } };
        if (!draw(g.arena_gtt + A_TSRC, TEST_N, TEST_N, TEST_N * 4, g.arena_gtt + A_TDST, TEST_N, TEST_N, TEST_N * 4,
                  I965_SURFACEFORMAT_B8G8R8A8_UNORM, 1, r, 2, "self-test"))
            return 0;
        flush_range(dst, TEST_N * TEST_N * 4);
        int bad = 0, first = -1;
        for (int py = 0; py < TEST_N; py++)
            for (int px = 0; px < TEST_N; px++) {
                u32 want = src[(TEST_N - 1 - px) * TEST_N + py];       /* rot 1: lx = py, ly = n-1-px */
                if ((dst[py * TEST_N + px] ^ want) & 0xffffff) { bad++; if (first < 0) first = py * TEST_N + px; }
            }
        if (!bad) return 1;
        klog("gpu: self-test %s: %d of %d pixels wrong (first %d: %08x)", g.clflush ? "with clflush" : "snooped",
             bad, TEST_N * TEST_N, first, dst[first]);
        if (g.clflush) break;
        g.clflush = 1;                                 /* maybe the GPU does not snoop the CPU caches */
        /* overwrite the source so a stale copy in memory cannot pass by luck */
        for (int i = 0; i < TEST_N * TEST_N; i++) src[i] ^= 0x00010101u;
    }
    return 0;
}

static int bring_up(void) {
    pci_dev_t *d = g.pci;
    u32 cmd = pci_read32(d->bus, d->dev, d->fn, 4);
    pci_write32(d->bus, d->dev, d->fn, 4, cmd | 0x6);                  /* memory space + bus master */
    u64 bar0 = pci_bar(d->bus, d->dev, d->fn, 0);
    g.gmadr = pci_bar(d->bus, d->dev, d->fn, 2);
    if (!bar0 || (bar0 & 0xffffff)) { fail("unexpected register BAR"); return 0; }
    if (bar0 + (16u << 20) > mm_max_phys()) { fail("registers outside the identity map"); return 0; }
    mm_uncached(bar0, 16u << 20);
    g.mmio = (volatile u8 *)(usize)bar0;
    g.gsm = (volatile u64 *)(usize)(bar0 + (8u << 20));               /* upper half of the 16 MiB BAR */

    u16 gmch = pci_read16(d->bus, d->dev, d->fn, 0x50);              /* SNB_GMCH_CTRL */
    u32 ggms = (gmch >> 8) & 3;                                       /* chv_get_total_gtt_size */
    if (!ggms) { fail("no GTT"); return 0; }
    g.ggtt_size = (u64)((1u << (20 + ggms)) / 8) << 12;

    /* the framebuffer: GOP put it in the aperture, so its GTT offset is known */
    if (!k.fb_base || !g.gmadr || k.fb_base < g.gmadr || k.fb_base - g.gmadr >= (1u << 30)) {
        fail("framebuffer is not in the GPU aperture");
        return 0;
    }
    g.fb_off = (u32)(k.fb_base - g.gmadr);
    if (!forcewake()) { fail("render power well did not wake"); return 0; }
    u32 acntr = rd(DSPACNTR), bcntr = rd(DSPBCNTR), asurf = rd(DSPASURF), bsurf = rd(DSPBSURF);
    klog("gpu: GTT %llu MB, fb at GTT %x; plane A %08x @%x, plane B %08x @%x",
         g.ggtt_size >> 20, g.fb_off, acntr, asurf, bcntr, bsurf);
    u32 cntr = (bsurf & ~0xfffu) == g.fb_off && (bcntr & DISPPLANE_ENABLE) ? bcntr : acntr;
    if (cntr & DISPPLANE_TILED) { fail("the framebuffer is tiled"); return 0; }
    usize fb_bytes = (usize)k.fb_stride * 4 * k.fb_h;
    if (g.ggtt_size < (64u << 20) || g.fb_off + fb_bytes > g.ggtt_size - SCENE_WINDOW - (1u << 20)) {
        fail("GTT too small");
        return 0;
    }

    /* our memory, at the very top of the GGTT; the canvas window below it */
    g.arena = hal_dma_alloc(ARENA_SIZE);
    if (!g.arena) { fail("out of memory"); return 0; }
    g.arena_gtt = (u32)(g.ggtt_size - (1u << 20));
    g.scene_gtt = g.arena_gtt - SCENE_WINDOW;
    static_state();
    u32 *gb = (u32 *)(g.arena + A_GOLDEN);
    memcpy(gb, qrt_golden_batch, sizeof qrt_golden_batch);
    for (usize i = 0; i < ARRAY_LEN(qrt_golden_relocs); i++) {
        u64 a = (u64)gb[qrt_golden_relocs[i] / 4] + g.arena_gtt + A_GOLDEN;
        gb[qrt_golden_relocs[i] / 4] = (u32)a;
        gb[qrt_golden_relocs[i] / 4 + 1] = (u32)(a >> 32);
    }
    flush_range(g.arena, ARENA_SIZE);
    gtt_map(g.arena_gtt, (u64)(usize)g.arena, ARENA_SIZE / 4096);

    /* PPAT: entry 0 (all GGTT traffic) snoops the CPU caches */
    wr(GEN8_PRIVATE_PAT_LO, CHV_PPAT_SNOOP);
    wr(GEN8_PRIVATE_PAT_HI, CHV_PPAT_SNOOP | CHV_PPAT_SNOOP << 8 | CHV_PPAT_SNOOP << 16 | CHV_PPAT_SNOOP << 24);

    workarounds();
    if (!ring_init()) { fail("the render ring did not start"); return 0; }
    if (!run_batch(g.arena_gtt + A_GOLDEN, "the null render state")) return 0;
    if (!self_test()) { if (g.state != G_FAILED) fail("self-test picture was wrong"); return 0; }
    return 1;
}

int gpu_probe(pci_dev_t *d) {
    if (!k.native || d->vendor != 0x8086 || (d->device & 0xfffc) != 0x22b0) return 0;
    g.pci = d;
    if (hal_setting_get(u"QrtGpuGuard", 0)) {
        /* the last start never came back: leave the GPU alone this time */
        hal_setting_set(u"QrtGpuGuard", 0);
        hal_setting_set(u"QrtGpu", 1);
        g.state = G_OFF;
        strlcpy(g.status, "Off: the last start did not finish", sizeof g.status);
        klog("gpu: %s", g.status);
        return 0;
    }
    if (hal_setting_get(u"QrtGpu", 0) == 1) {
        g.state = G_OFF;
        strlcpy(g.status, "Off (CPU drawing)", sizeof g.status);
        return 0;
    }
    hal_setting_set(u"QrtGpuGuard", 1);
    g.guard = 1;
    u64 t0 = k_now_us();
    int ok = bring_up();
    if (!ok) {
        hal_setting_set(u"QrtGpuGuard", 0);
        g.guard = 0;
        if (g.state != G_FAILED) fail("start failed");
        return 0;
    }
    g.state = G_READY;
    fmt(g.status, sizeof g.status, "On: 3D engine draws the screen (%s)", g.clflush ? "cache flushes" : "coherent");
    klog("gpu: render engine up in %llu us, self-test passed (%s)", k_now_us() - t0,
         g.clflush ? "GPU reads need clflush" : "snooped");
    return 1;
}

int gpu_supported(void) { return g.pci != NULL; }
int gpu_active(void) { return g.state == G_READY; }
int gpu_enabled(void) { return g.pci && hal_setting_get(u"QrtGpu", 0) != 1; }
const char *gpu_status(void) { return g.pci ? g.status : "No supported GPU (CPU drawing)"; }

void gpu_set_enabled(int on) {
    if (!g.pci) return;
    hal_setting_set(u"QrtGpu", on ? 0 : 1);
    if (!on) {
        if (g.state == G_READY) g.state = G_OFF;
        strlcpy(g.status, "Off (CPU drawing)", sizeof g.status);
    } else if (g.state == G_OFF) {
        if (g.arena) {                                 /* already set up once */
            g.state = G_READY;
            fmt(g.status, sizeof g.status, "On: 3D engine draws the screen (%s)", g.clflush ? "cache flushes" : "coherent");
        } else gpu_probe(g.pci);
    }
}

void gpu_stats(u32 *frames, u32 *avg_us, int *coherent) {
    *frames = g.frames;
    *avg_us = g.frames ? (u32)(g.busy_us / g.frames) : 0;
    *coherent = !g.clflush;
}

/* The GGTT address of a canvas, entering its pages when it is new.  Two
 * slots, so the shell's scene and frame canvases both stay mapped. */
static u32 map_canvas(const u32 *src, usize bytes) {
    const u8 *first = (const u8 *)((usize)src & ~(usize)4095);
    usize pages = ((usize)src + bytes - (usize)first + 4095) / 4096;
    if (pages + 1 > SLOT_SIZE / 4096) return 0;
    int s = 0;
    for (int i = 0; i < 2; i++)
        if (g.slot[i].page == first && g.slot[i].pages == pages) { s = i; goto found; }
    s = g.slot[0].used <= g.slot[1].used ? 0 : 1;            /* the one used longest ago */
    u32 base = g.scene_gtt + (u32)s * SLOT_SIZE;
    gtt_map(base, (u64)(usize)first, pages);
    gtt_map(base + (u32)pages * 4096, (u64)(usize)(g.arena + A_ZERO), 1);
    g.slot[s].page = first;
    g.slot[s].pages = pages;
found:
    g.slot[s].used = ++g.clock;
    return g.scene_gtt + (u32)s * SLOT_SIZE + (u32)((usize)src & 4095);
}

int gpu_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) {
    if (g.state != G_READY || w <= 0 || h <= 0) return 0;
    u32 src_gtt = map_canvas(src, (usize)stride * 4 * sh);
    if (!src_gtt) return 0;
    int fw = (int)k.fb_w, fh = (int)k.fb_h;
    grect_t r;
    switch (rot & 3) {
    case 0: r = (grect_t){ x, y, w, h }; break;
    case 1: r = (grect_t){ fw - (y + h), x, h, w }; break;
    case 2: r = (grect_t){ fw - (x + w), fh - (y + h), w, h }; break;
    default: r = (grect_t){ y, fh - (x + w), h, w }; break;
    }
    if (g.clflush)
        for (int ly = y; ly < y + h; ly++) flush_range(src + (usize)ly * stride + x, (usize)w * 4);
    if (!draw(src_gtt, sw, sh, stride * 4, g.fb_off, fw, fh, (int)k.fb_stride * 4,
              k.fb_rgb ? I965_SURFACEFORMAT_R8G8B8A8_UNORM : I965_SURFACEFORMAT_B8G8R8A8_UNORM, rot, &r, 1, "a frame"))
        return 0;
    if (++g.frames == 300 && g.guard) {                /* it works: disarm the crash guard */
        hal_setting_set(u"QrtGpuGuard", 0);
        g.guard = 0;
    }
    return 1;
}

#else   /* 32-bit builds run on the firmware only */
int gpu_probe(pci_dev_t *d) { (void)d; return 0; }
int gpu_supported(void) { return 0; }
int gpu_active(void) { return 0; }
int gpu_enabled(void) { return 0; }
const char *gpu_status(void) { return "Needs the 64-bit native kernel"; }
void gpu_set_enabled(int on) { (void)on; }
void gpu_stats(u32 *frames, u32 *avg_us, int *coherent) { *frames = 0; *avg_us = 0; *coherent = 0; }
int gpu_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) { return 0; }
#endif
