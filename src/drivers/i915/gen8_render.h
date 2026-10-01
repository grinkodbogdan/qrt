/*
 * gen8_render.h - Gen8 3D pipeline state layouts and command opcodes.
 *
 * The structures and numbers come from intel-vaapi-driver (src/i965_structs.h,
 * src/i965_defines.h; Copyright (c) Intel Corporation, MIT licence) and the
 * MI/register numbers from Linux's drivers/gpu/drm/i915/i915_reg.h (v4.9,
 * MIT licence).  Only what QRT's blit path needs is kept.
 */
#pragma once

/* ---- state blocks the GPU reads from memory -------------------------------- */
struct gen8_surface_state {
    struct {
        unsigned int cube_pos_z: 1, cube_neg_z: 1, cube_pos_y: 1, cube_neg_y: 1, cube_pos_x: 1, cube_neg_x: 1;
        unsigned int media_boundary_pixel_mode: 2;
        unsigned int render_cache_read_write: 1;
        unsigned int sampler_l2bypass_disable: 1;
        unsigned int vert_line_stride_ofs: 1;
        unsigned int vert_line_stride: 1;
        unsigned int tile_walk: 1;
        unsigned int tiled_surface: 1;
        unsigned int horizontal_alignment: 2;
        unsigned int vertical_alignment: 2;
        unsigned int surface_format: 9;
        unsigned int pad0: 1;
        unsigned int is_array: 1;
        unsigned int surface_type: 3;
    } ss0;
    struct { unsigned int surface_qpitch: 15, pad0: 4, base_mip_level: 5, surface_mocs: 7, pad1: 1; } ss1;
    struct { unsigned int width: 14, pad0: 2, height: 14, pad1: 2; } ss2;
    struct { unsigned int pitch: 18, pad: 3, depth: 11; } ss3;
    struct {
        unsigned int multisample_position_palette_index: 3, num_multisamples: 3, multisampled_surface_storage_format: 1;
        unsigned int render_target_view_extent: 11, min_array_elt: 11, rotation: 2, force_ncmp_reduce_type: 1;
    } ss4;
    struct {
        unsigned int mip_count: 4, min_lod: 4, pad0: 4, pad1: 2, coherence_type: 1, pad2: 3, pad3: 2;
        unsigned int ewa_disable_cube: 1, y_offset: 3, pad4: 1, x_offset: 7;
    } ss5;
    struct { unsigned int y_offset_uv_plane: 14, pad0: 2, x_offset_uv_plane: 14, pad1: 1, separate_uv_plane: 1; } ss6;
    struct {
        unsigned int resource_min_lod: 12, pad0: 4;
        unsigned int shader_chanel_select_a: 3, shader_chanel_select_b: 3, shader_chanel_select_g: 3, shader_chanel_select_r: 3;
        unsigned int alpha_clear_color: 1, blue_clear_color: 1, green_clear_color: 1, red_clear_color: 1;
    } ss7;
    struct { unsigned int base_addr; } ss8;
    struct { unsigned int base_addr_high: 16, pad0: 16; } ss9;
    unsigned int ss10_15[6];
};

struct gen8_sampler_state {
    struct {
        unsigned int aniso_algorithm: 1, lod_bias: 13, min_filter: 3, mag_filter: 3, mip_filter: 2;
        unsigned int base_level: 5, lod_preclamp: 2, default_color_mode: 1, pad0: 1, disable: 1;
    } ss0;
    struct {
        unsigned int cube_control_mode: 1, shadow_function: 3, chroma_key_mode: 1, chroma_key_index: 2;
        unsigned int chroma_key_enable: 1, max_lod: 12, min_lod: 12;
    } ss1;
    unsigned int ss2;
    struct {
        unsigned int r_wrap_mode: 3, t_wrap_mode: 3, s_wrap_mode: 3, pad0: 1, non_normalized_coord: 1;
        unsigned int trilinear_quality: 2, address_round: 6, max_aniso: 3, pad1: 2, nonsep_filter_foot_lowmask: 8;
    } ss3;
};

struct gen8_global_blend_state {
    unsigned int pad0: 19, ydither_offset: 2, xdither_offset: 2, color_dither_enable: 1;
    unsigned int alpha_test_func: 3, alpha_test_enable: 1, alpha_to_coverage_dither: 1, alpha_to_one: 1;
    unsigned int ia_blend_enable: 1, alpha_to_coverage: 1;
};

struct gen8_blend_state_rt {
    struct {
        unsigned int blue_write_dis: 1, green_write_dis: 1, red_write_dis: 1, alpha_write_dis: 1, pad0: 1;
        unsigned int alpha_blend_func: 3, ia_dest_blend_factor: 5, ia_src_blend_factor: 5;
        unsigned int color_blend_func: 3, dest_blend_factor: 5, src_blend_factor: 5, colorbuf_blend: 1;
    } blend0;
    struct {
        unsigned int post_blend_clamp_enable: 1, pre_blend_clamp_enable: 1, clamp_range: 2, pre_blend_src_clamp: 1;
        unsigned int pad0: 22, logic_op_func: 4, logic_op_enable: 1;
    } blend1;
};

struct i965_cc_viewport { float min_depth, max_depth; };

struct gen6_color_calc_state {
    unsigned int cc0;
    float alpha_ref;
    float constant_r, constant_g, constant_b, constant_a;
};

/* ---- 3D commands ------------------------------------------------------------ */
#define CMD(pipeline, op, sub_op) ((3u << 29) | ((pipeline) << 27) | ((op) << 24) | ((sub_op) << 16))

#define CMD_PIPELINE_SELECT                     CMD(1, 1, 4)
#define   PIPELINE_SELECT_3D                    0
#define CMD_STATE_BASE_ADDRESS                  CMD(0, 1, 1)
#define   BASE_ADDRESS_MODIFY                   (1 << 0)
#define CMD_STATE_SIP                           CMD(0, 1, 2)
#define CMD_VERTEX_BUFFERS                      CMD(3, 0, 8)
#define CMD_VERTEX_ELEMENTS                     CMD(3, 0, 9)
#define CMD_DRAWING_RECTANGLE                   CMD(3, 1, 0)
#define CMD_3DPRIMITIVE                         CMD(3, 3, 0)

#define GEN6_3DSTATE_CC_STATE_POINTERS          CMD(3, 0, 0x0e)
#define GEN6_3DSTATE_VS                         CMD(3, 0, 0x10)
#define GEN6_3DSTATE_GS                         CMD(3, 0, 0x11)
#define GEN6_3DSTATE_CLIP                       CMD(3, 0, 0x12)
#define GEN6_3DSTATE_SF                         CMD(3, 0, 0x13)
#define GEN6_3DSTATE_WM                         CMD(3, 0, 0x14)
#define GEN6_3DSTATE_CONSTANT_VS                CMD(3, 0, 0x15)
#define GEN6_3DSTATE_CONSTANT_GS                CMD(3, 0, 0x16)
#define GEN6_3DSTATE_CONSTANT_PS                CMD(3, 0, 0x17)
#define GEN6_3DSTATE_SAMPLE_MASK                CMD(3, 0, 0x18)
#define GEN7_3DSTATE_CONSTANT_HS                CMD(3, 0, 0x19)
#define GEN7_3DSTATE_CONSTANT_DS                CMD(3, 0, 0x1a)
#define GEN7_3DSTATE_HS                         CMD(3, 0, 0x1b)
#define GEN7_3DSTATE_TE                         CMD(3, 0, 0x1c)
#define GEN7_3DSTATE_DS                         CMD(3, 0, 0x1d)
#define GEN7_3DSTATE_STREAMOUT                  CMD(3, 0, 0x1e)
#define GEN7_3DSTATE_SBE                        CMD(3, 0, 0x1f)
#define GEN7_3DSTATE_PS                         CMD(3, 0, 0x20)
#define GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CL CMD(3, 0, 0x21)
#define GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC CMD(3, 0, 0x23)
#define GEN7_3DSTATE_BLEND_STATE_POINTERS       CMD(3, 0, 0x24)
#define GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS  CMD(3, 0, 0x26)
#define GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS  CMD(3, 0, 0x27)
#define GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS  CMD(3, 0, 0x28)
#define GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS  CMD(3, 0, 0x29)
#define GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS  CMD(3, 0, 0x2a)
#define GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS  CMD(3, 0, 0x2b)
#define GEN7_3DSTATE_SAMPLER_STATE_POINTERS_HS  CMD(3, 0, 0x2c)
#define GEN7_3DSTATE_SAMPLER_STATE_POINTERS_DS  CMD(3, 0, 0x2d)
#define GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS  CMD(3, 0, 0x2e)
#define GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS  CMD(3, 0, 0x2f)
#define GEN7_3DSTATE_URB_VS                     CMD(3, 0, 0x30)
#define GEN7_3DSTATE_URB_HS                     CMD(3, 0, 0x31)
#define GEN7_3DSTATE_URB_DS                     CMD(3, 0, 0x32)
#define GEN7_3DSTATE_URB_GS                     CMD(3, 0, 0x33)
#define GEN7_3DSTATE_DEPTH_BUFFER               CMD(3, 0, 0x05)
#define GEN7_3DSTATE_STENCIL_BUFFER             CMD(3, 0, 0x06)
#define GEN7_3DSTATE_HIER_DEPTH_BUFFER          CMD(3, 0, 0x07)
#define GEN7_3DSTATE_CLEAR_PARAMS               CMD(3, 0, 0x04)
#define GEN8_3DSTATE_MULTISAMPLE                CMD(3, 0, 0x0d)
#define GEN8_3DSTATE_VF_INSTANCING              CMD(3, 0, 0x49)
#define GEN8_3DSTATE_VF_SGVS                    CMD(3, 0, 0x4a)
#define GEN8_3DSTATE_VF_TOPOLOGY                CMD(3, 0, 0x4b)
#define GEN8_3DSTATE_PSBLEND                    CMD(3, 0, 0x4d)
#define GEN8_3DSTATE_WM_DEPTH_STENCIL           CMD(3, 0, 0x4e)
#define GEN8_3DSTATE_PSEXTRA                    CMD(3, 0, 0x4f)
#define GEN8_3DSTATE_RASTER                     CMD(3, 0, 0x50)
#define GEN8_3DSTATE_SBE_SWIZ                   CMD(3, 0, 0x51)
#define GEN8_3DSTATE_WM_HZ_OP                   CMD(3, 0, 0x52)
#define GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS     CMD(3, 1, 0x12)
#define GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_HS     CMD(3, 1, 0x13)
#define GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_DS     CMD(3, 1, 0x14)
#define GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_GS     CMD(3, 1, 0x15)
#define GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS     CMD(3, 1, 0x16)
#define GEN8_3DSTATE_SAMPLE_PATTERN             CMD(3, 1, 0x1c)

#define GEN6_3DSTATE_MULTISAMPLE_PIXEL_LOCATION_CENTER (0 << 4)
#define GEN6_3DSTATE_MULTISAMPLE_NUMSAMPLES_1   (0 << 1)
#define GEN8_VB0_BUFFER_INDEX_SHIFT             26
#define GEN8_VB0_MOCS_SHIFT                     16
#define GEN7_VB0_ADDRESS_MODIFYENABLE           (1 << 14)
#define VB0_BUFFER_PITCH_SHIFT                  0
#define GEN8_VE0_VERTEX_BUFFER_INDEX_SHIFT      26
#define GEN8_VE0_VALID                          (1 << 25)
#define VE0_FORMAT_SHIFT                        16
#define VE0_OFFSET_SHIFT                        0
#define VE1_VFCOMPONENT_0_SHIFT                 28
#define VE1_VFCOMPONENT_1_SHIFT                 24
#define VE1_VFCOMPONENT_2_SHIFT                 20
#define VE1_VFCOMPONENT_3_SHIFT                 16
#define I965_VFCOMPONENT_STORE_SRC              1
#define I965_VFCOMPONENT_STORE_0                2
#define I965_VFCOMPONENT_STORE_1_FLT            3
#define _3DPRIM_RECTLIST                        0x0f
#define GEN7_3DPRIM_VERTEXBUFFER_ACCESS_SEQUENTIAL (0 << 8)
#define GEN7_URB_ENTRY_NUMBER_SHIFT             0
#define GEN7_URB_ENTRY_SIZE_SHIFT               16
#define GEN7_URB_STARTING_ADDRESS_SHIFT         25
#define GEN8_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT  16
#define GEN8_PUSH_CONSTANT_BUFFER_SIZE_SHIFT    0
#define GEN8_3DSTATE_RASTER_CULL_NONE           (1 << 16)
#define GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH    (1 << 29)
#define GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET    (1 << 28)
#define GEN7_SBE_NUM_OUTPUTS_SHIFT              22
#define GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT    11
#define GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT    5
#define GEN6_3DSTATE_SF_TRIFAN_PROVOKE_SHIFT    25
#define GEN8_PSX_PIXEL_SHADER_VALID             (1u << 31)
#define GEN8_PSX_ATTRIBUTE_ENABLE               (1 << 8)
#define GEN8_PS_BLEND_HAS_WRITEABLE_RT          (1 << 30)
#define GEN7_WM_PERSPECTIVE_PIXEL_BARYCENTRIC   (1 << 11)
#define GEN7_PS_SAMPLER_COUNT_SHIFT             27
#define GEN7_PS_BINDING_TABLE_ENTRY_COUNT_SHIFT 18
#define GEN7_PS_VECTOR_MASK_ENABLE              (1 << 30)
#define GEN8_PS_MAX_THREADS_SHIFT               23
#define GEN7_PS_PUSH_CONSTANT_ENABLE            (1 << 11)
#define GEN7_PS_16_DISPATCH_ENABLE              (1 << 1)
#define GEN7_PS_DISPATCH_START_GRF_SHIFT_0      16
#define I965_DEPTHFORMAT_D32_FLOAT              1

#define I965_SURFACE_2D                         1
#define I965_SURFACE_NULL                       7
#define I965_SURFACEFORMAT_R32G32_FLOAT         0x085
#define I965_SURFACEFORMAT_B8G8R8A8_UNORM       0x0c0
#define I965_SURFACEFORMAT_R8G8B8A8_UNORM       0x0c7
#define I965_MAPFILTER_NEAREST                  0x0
#define I965_TEXCOORDMODE_CLAMP                 2
#define HSW_SCS_RED                             4
#define HSW_SCS_GREEN                           5
#define HSW_SCS_BLUE                            6
#define HSW_SCS_ALPHA                           7

/* ---- MI commands (i915_reg.h) ------------------------------------------------ */
#define MI_INSTR(op, flags)                     (((op) << 23) | (flags))
#define MI_NOOP                                 MI_INSTR(0, 0)
#define MI_BATCH_BUFFER_END                     MI_INSTR(0x0a, 0)
#define MI_BATCH_BUFFER_START_GEN8              MI_INSTR(0x31, 1)
#define GFX_OP_PIPE_CONTROL(len)                ((0x3u << 29) | (0x3 << 27) | (0x2 << 24) | ((len) - 2))
#define   PIPE_CONTROL_GLOBAL_GTT_IVB           (1 << 24)
#define   PIPE_CONTROL_CS_STALL                 (1 << 20)
#define   PIPE_CONTROL_TLB_INVALIDATE           (1 << 18)
#define   PIPE_CONTROL_QW_WRITE                 (1 << 14)
#define   PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH (1 << 12)
#define   PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE (1 << 11)
#define   PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE (1 << 10)
#define   PIPE_CONTROL_FLUSH_ENABLE             (1 << 7)
#define   PIPE_CONTROL_DC_FLUSH_ENABLE          (1 << 5)
#define   PIPE_CONTROL_VF_CACHE_INVALIDATE      (1 << 4)
#define   PIPE_CONTROL_CONST_CACHE_INVALIDATE   (1 << 3)
#define   PIPE_CONTROL_STATE_CACHE_INVALIDATE   (1 << 2)
#define   PIPE_CONTROL_STALL_AT_SCOREBOARD      (1 << 1)
#define   PIPE_CONTROL_DEPTH_CACHE_FLUSH        (1 << 0)

/* ---- registers (offsets into the MMIO half of BAR 0) -------------------------- */
#define RCS_TAIL            0x2030
#define RCS_HEAD            0x2034
#define RCS_START           0x2038
#define RCS_CTL             0x203c
#define   RING_NR_PAGES     0x001ff000
#define   RING_VALID        0x00000001
#define RCS_IPEHR           0x2068
#define RCS_ACTHD           0x2074
#define RCS_HWS_PGA         0x2080
#define RCS_MI_MODE         0x209c
#define   MODE_IDLE         (1 << 9)
#define   STOP_RING         (1 << 8)
#define   ASYNC_FLIP_PERF_DISABLE (1 << 14)
#define RCS_INSTPM          0x20c0
#define   INSTPM_FORCE_ORDERING (1 << 7)
#define EIR                 0x20b0
#define GEN7_FF_THREAD_MODE 0x20a0
#define   GEN8_FF_DS_REF_CNT_FFME (1 << 19)
#define   GEN7_FF_VS_REF_CNT_FFME (1 << 15)
#define GEN6_RC_SLEEP_PSMI_CONTROL 0x2050
#define   GEN8_RC_SEMA_IDLE_MSG_DISABLE (1 << 12)
#define GFX_MODE_GEN7       0x229c
#define   GFX_RUN_LIST_ENABLE (1 << 15)
#define HSW_GTT_CACHE_EN    0x4024
#define   GTT_CACHE_EN_ALL  0xf0007fff
#define ERROR_GEN6          0x40a0
#define GEN8_PRIVATE_PAT_LO 0x40e0
#define GEN8_PRIVATE_PAT_HI 0x40e4
#define   CHV_PPAT_SNOOP    (1 << 6)
#define CACHE_MODE_0_GEN7   0x7000
#define   HIZ_RAW_STALL_OPT_DISABLE (1 << 2)
#define CACHE_MODE_1        0x7004
#define   GEN8_4x4_STC_OPTIMIZATION_DISABLE (1 << 6)
#define GEN7_GT_MODE        0x7008
#define   GEN6_WIZ_HASHING_MASK  ((1 << 9) | (1 << 7))
#define   GEN6_WIZ_HASHING_16x4  (1 << 9)
#define HIZ_CHICKEN         0x7018
#define   CHV_HZ_8X8_MODE_IN_1X (1 << 15)
#define HDC_CHICKEN0        0x7300
#define   HDC_DONOT_FETCH_MEM_WHEN_MASKED (1 << 11)
#define   HDC_FORCE_NON_COHERENT (1 << 4)
#define GEN6_UCGCTL1        0x9400
#define   GEN6_CSUNIT_CLOCK_GATE_DISABLE (1 << 7)
#define GEN7_MISCCPCTL      0x9424
#define   GEN7_DOP_CLOCK_GATE_ENABLE (1 << 0)
#define GEN8_UCGCTL6        0x9430
#define   GEN8_SDEUNIT_CLOCK_GATE_DISABLE (1 << 14)
#define GEN6_GDRST          0x941c
#define   GEN6_GRDOM_FULL   (1 << 0)
#define GEN8_L3SQCREG1      0xb100
#define GEN8_ROW_CHICKEN    0xe4f0
#define   PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE (1 << 8)
#define   STALL_DOP_GATING_DISABLE (1 << 5)
#define GFX_FLSH_CNTL_GEN6  0x101008
#define VLV_GTLC_WAKE_CTRL  0x130090
#define   VLV_GTLC_ALLOWWAKEREQ (1 << 0)
#define VLV_GTLC_PW_STATUS  0x130094
#define FORCEWAKE_VLV       0x1300b0
#define FORCEWAKE_ACK_VLV   0x1300b4
#define   FORCEWAKE_KERNEL  0x1
#define VLV_DISPLAY_BASE    0x180000
#define DSPACNTR            (VLV_DISPLAY_BASE + 0x70180)
#define DSPASURF            (VLV_DISPLAY_BASE + 0x7019c)
#define DSPBCNTR            (VLV_DISPLAY_BASE + 0x71180)
#define DSPBSURF            (VLV_DISPLAY_BASE + 0x7119c)
#define   DISPPLANE_ENABLE  (1u << 31)
#define   DISPPLANE_TILED   (1 << 10)

#define MASKED_ENABLE(b)    (((b) << 16) | (b))
#define MASKED_DISABLE(b)   ((b) << 16)
#define MASKED_FIELD(m, v)  (((m) << 16) | (v))
