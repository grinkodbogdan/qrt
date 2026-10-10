/*
 * qrtcc - QRT's offline compiler for the Mi A1's GPU (Adreno 506).
 *
 * GLSL compute shaders (compiled to SPIR-V by glslang) go through Mesa's own front end
 * (spirv_to_nir) and Adreno back end (ir3) - the compiler Mesa's freedreno driver uses -
 * and come out as a C header: each kernel's machine code and what QRT needs to run it
 * (src/arch/arm64/lgpu.c): register footprint, constant layout, local size, the
 * registers the hardware puts the invocation and work-group IDs in.
 *
 * Buffers are SSBOs by binding (set 0); parameters are push constants, read straight
 * from the constant file (as Mesa's Vulkan driver does).
 *
 *   qrtcc out.h name1 file1.spv [name2 file2.spv ...]
 *
 * Built against a Mesa build tree (tools/adreno/build.sh); not part of QRT's build.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/glsl_types.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "common/freedreno_dev_info.h"

static uint32_t push_base_dw;            /* where push constants start in the const file */

static bool lower_intr(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   (void)data;
   b->cursor = nir_before_instr(&in->instr);
   switch (in->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      nir_def *idx = nir_iadd_imm(b, in->src[0].ssa, nir_intrinsic_binding(in));
      nir_def_replace(&in->def, nir_vec2(b, idx, nir_imm_int(b, 0)));
      return true;
   }
   case nir_intrinsic_load_vulkan_descriptor:
      nir_def_replace(&in->def, in->src[0].ssa);
      return true;
   case nir_intrinsic_load_push_constant: {
      uint32_t base = nir_intrinsic_base(in);
      nir_def *l = nir_load_const_ir3(b, in->num_components, in->def.bit_size,
                                      nir_ushr_imm(b, in->src[0].ssa, 2), .base = push_base_dw + base / 4);
      nir_def_replace(&in->def, l);
      return true;
   }
   default:
      return false;
   }
}

static uint32_t *read_file(const char *p, size_t *n)
{
   FILE *f = fopen(p, "rb");
   if (!f) { perror(p); exit(1); }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *w = malloc(sz);
   if (fread(w, 1, sz, f) != (size_t)sz) { perror(p); exit(1); }
   fclose(f);
   *n = sz / 4;
   return w;
}

static void compile(FILE *out, struct ir3_compiler *c, const char *name, const char *path)
{
   size_t nw;
   uint32_t *words = read_file(path, &nw);
   const nir_shader_compiler_options *nopts = ir3_get_compiler_options(c);
   struct spirv_to_nir_options spv = {
      .environment = NIR_SPIRV_VULKAN,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
   };
   nir_shader *nir = spirv_to_nir(words, nw, NULL, 0, MESA_SHADER_COMPUTE, "main", &spv, nopts);
   if (!nir) { fprintf(stderr, "%s: spirv_to_nir failed\n", path); exit(1); }

   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_copy_prop);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_system_values);
   nir_lower_compute_system_values_options csv = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv);
   /* SSBOs by binding: the hardware's buffer slots (num_ssbos is not set on the SPIR-V path) */
   unsigned nssbo = 0;
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ssbo) {
      unsigned n = var->data.binding + MAX2(1, glsl_get_aoa_size(var->type));
      if (n > nssbo) nssbo = n;
   }
   nir->info.num_ssbos = nssbo;
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo | nir_var_mem_ubo, nir_address_format_32bit_index_offset);

   /* push constants: as many as the shader reads */
   uint32_t push_dw = 0;
   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(blk, impl)
         nir_foreach_instr(ins, blk) {
            if (ins->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(ins);
            if (in->intrinsic != nir_intrinsic_load_push_constant) continue;
            uint32_t end = (nir_intrinsic_base(in) + nir_intrinsic_range(in) + 3) / 4;
            if (end > push_dw) push_dw = end;
         }
   push_base_dw = 0;
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_intr, nir_metadata_control_flow, NULL);
   /* the buffers are indices and offsets now: their variables can go */
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_mem_ssbo | nir_var_mem_ubo | nir_var_mem_push_const, NULL);
   nir_foreach_variable_with_modes_safe(var, nir, nir_var_mem_ssbo | nir_var_mem_ubo | nir_var_mem_push_const)
      exec_node_remove(&var->node);

   struct ir3_shader_nir_options nir_opts = { 0 };
   ir3_finalize_nir(c, &nir_opts, nir);

   struct ir3_shader_options so = { 0 };
   so.api_wavesize = IR3_SINGLE_OR_DOUBLE;
   so.real_wavesize = IR3_SINGLE_OR_DOUBLE;
   so.push_consts_type = IR3_PUSH_CONSTS_PER_STAGE;
   so.push_consts_base = 0;
   so.push_consts_dwords = push_dw;
   if (push_dw) ir3_const_alloc(&so.const_allocs, IR3_CONST_ALLOC_PUSH_CONSTS, (push_dw + 3) / 4, 1);
   so.nir_options = nir_opts;
   struct ir3_shader *sh = ir3_shader_from_nir(c, nir, &so, NULL);
   struct ir3_shader_key key = { 0 };
   struct ir3_shader_variant *v = ir3_shader_create_variant(sh, &key, false);
   if (!v || !v->bin) { fprintf(stderr, "%s: ir3 failed\n", path); exit(1); }

   const struct ir3_info *i = &v->info;
   const struct ir3_const_state *cs = ir3_const_state(v);
   if (v->pvtmem_size) { fprintf(stderr, "%s: needs private memory (%u) - not supported\n", path, v->pvtmem_size); exit(1); }
   if (cs->num_driver_params) fprintf(stderr, "%s: warning: %u driver params (not filled by QRT)\n", path, cs->num_driver_params);
   uint32_t imm_base = cs->allocs.max_const_offset_vec4;
   uint32_t pc_off = push_dw ? cs->allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].offset_vec4 : 0;

   fprintf(out, "/* %s: %u instructions, %u full / %u half registers */\n", name, i->instrs_count,
           i->max_reg + 1, i->max_half_reg + 1);
   fprintf(out, "static const u32 gk_%s_bin[] = {", name);
   for (unsigned k = 0; k < i->sizedwords; k++) fprintf(out, "%s0x%08x,", k % 8 ? " " : "\n   ", ((uint32_t *)v->bin)[k]);
   fprintf(out, "\n};\n");
   fprintf(out, "static const u32 gk_%s_imm[] = {", name);
   for (unsigned k = 0; k < v->imm_state.count; k++) fprintf(out, "%s0x%08x,", k % 8 ? " " : "\n   ", v->imm_state.values[k]);
   fprintf(out, " 0 };\n");
   fprintf(out,
           "static const struct gkernel gk_%s = { \"%s\", gk_%s_bin, %u, %u, %u, %u, %u, %u, %u, { %u, %u, %u },\n"
           "   %u, %u, %u, %u, gk_%s_imm, %u, %u };\n\n",
           name, name, name, i->sizedwords, v->instrlen, i->max_reg + 1, i->max_half_reg + 1,
           i->double_threadsize, ir3_shader_branchstack_hw(v), v->constlen,
           v->local_size[0], v->local_size[1], v->local_size[2],
           ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID),
           ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID),
           pc_off * 4, push_dw, name, imm_base * 4, v->imm_state.count);
   fprintf(stderr, "%s: %u instrs, regs %u/%u, consts %u vec4 (push at %u, %u dw; imm at %u, %u dw), local %ux%ux%u, ids r%u c%u\n",
           name, i->instrs_count, i->max_reg + 1, i->max_half_reg + 1, v->constlen, pc_off * 4, push_dw, imm_base * 4,
           v->imm_state.count, v->local_size[0], v->local_size[1], v->local_size[2],
           ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID), ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID));
   if (getenv("QRTCC_DISASM")) ir3_shader_disasm(v, (uint32_t *)v->bin, stderr);
}

int main(int argc, char **argv)
{
   if (argc < 4 || argc % 2) { fprintf(stderr, "usage: qrtcc out.h name file.spv [...]\n"); return 1; }
   glsl_type_singleton_init_or_ref();
   struct fd_dev_id id = { .gpu_id = 506 };
   struct ir3_compiler_options copts = { 0 };
   struct ir3_compiler *c = ir3_compiler_create(NULL, &id, fd_dev_info_raw(&id), &copts);
   if (!c) { fprintf(stderr, "no ir3 compiler for the Adreno 506\n"); return 1; }
   FILE *out = fopen(argv[1], "w");
   if (!out) { perror(argv[1]); return 1; }
   fprintf(out, "/* Generated by tools/adreno/qrtcc from src/arch/arm64/gpu/NAME.comp - do not edit.\n"
                " * Adreno 506 machine code compiled by Mesa's ir3 (MIT licence). */\n\n");
   for (int k = 2; k < argc; k += 2) compile(out, c, argv[k], argv[k + 1]);
   fclose(out);
   return 0;
}
