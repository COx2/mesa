/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef RADV_SHADER_H
#define RADV_SHADER_H

#include "ac_binary.h"
#include "ac_shader_util.h"

#include "amd_family.h"
#include "radv_constants.h"

#include "nir/nir.h"
#include "vulkan/runtime/vk_object.h"
#include "vulkan/runtime/vk_shader_module.h"
#include "vulkan/vulkan.h"

#include "aco_shader_info.h"

#define RADV_VERT_ATTRIB_MAX MAX2(VERT_ATTRIB_MAX, VERT_ATTRIB_GENERIC0 + MAX_VERTEX_ATTRIBS)

struct radv_physical_device;
struct radv_device;
struct radv_pipeline;
struct radv_pipeline_cache;
struct radv_pipeline_key;
struct radv_shader_args;
struct radv_vs_input_state;
struct radv_shader_args;

struct radv_pipeline_key {
   uint32_t has_multiview_view_index : 1;
   uint32_t optimisations_disabled : 1;
   uint32_t invariant_geom : 1;
   uint32_t use_ngg : 1;
   uint32_t adjust_frag_coord_z : 1;
   uint32_t disable_aniso_single_level : 1;
   uint32_t disable_sinking_load_input_fs : 1;
   uint32_t image_2d_view_of_3d : 1;
   uint32_t primitives_generated_query : 1;
   uint32_t dynamic_patch_control_points : 1;

   struct {
      uint32_t instance_rate_inputs;
      uint32_t instance_rate_divisors[MAX_VERTEX_ATTRIBS];
      uint8_t vertex_attribute_formats[MAX_VERTEX_ATTRIBS];
      uint32_t vertex_attribute_bindings[MAX_VERTEX_ATTRIBS];
      uint32_t vertex_attribute_offsets[MAX_VERTEX_ATTRIBS];
      uint32_t vertex_attribute_strides[MAX_VERTEX_ATTRIBS];
      uint8_t vertex_binding_align[MAX_VBS];
      uint32_t provoking_vtx_last : 1;
      uint32_t dynamic_input_state : 1;
      uint8_t topology;
   } vs;

   struct {
      unsigned tess_input_vertices;
   } tcs;

   struct {
      uint32_t col_format;
      uint32_t is_int8;
      uint32_t is_int10;
      uint32_t cb_target_mask;
      uint8_t log2_ps_iter_samples;
      uint8_t num_samples;
      bool mrt0_is_dual_src;

      bool lower_discard_to_demote;
      uint8_t enable_mrt_output_nan_fixup;
      bool force_vrs_enabled;

      /* Used to export alpha through MRTZ for alpha-to-coverage (GFX11+). */
      bool alpha_to_coverage_via_mrtz;

      bool has_epilog;
   } ps;

   struct {
      /* Non-zero if a required subgroup size is specified via
       * VK_EXT_subgroup_size_control.
       */
      uint8_t compute_subgroup_size;
      bool require_full_subgroups;
   } cs;
};

struct radv_nir_compiler_options {
   struct radv_pipeline_key key;
   bool robust_buffer_access;
   bool dump_shader;
   bool dump_preoptir;
   bool record_ir;
   bool record_stats;
   bool check_ir;
   bool has_ls_vgpr_init_bug;
   uint8_t enable_mrt_output_nan_fixup;
   bool wgp_mode;
   enum radeon_family family;
   enum amd_gfx_level gfx_level;
   uint32_t address32_hi;
   bool has_3d_cube_border_color_mipmap;

   struct {
      void (*func)(void *private_data, enum aco_compiler_debug_level level, const char *message);
      void *private_data;
   } debug;
};

enum radv_ud_index {
   AC_UD_SCRATCH_RING_OFFSETS = 0,
   AC_UD_PUSH_CONSTANTS = 1,
   AC_UD_INLINE_PUSH_CONSTANTS = 2,
   AC_UD_INDIRECT_DESCRIPTOR_SETS = 3,
   AC_UD_VIEW_INDEX = 4,
   AC_UD_STREAMOUT_BUFFERS = 5,
   AC_UD_NGG_QUERY_STATE = 6,
   AC_UD_NGG_CULLING_SETTINGS = 7,
   AC_UD_NGG_VIEWPORT = 8,
   AC_UD_FORCE_VRS_RATES = 9,
   AC_UD_TASK_RING_ENTRY = 10,
   AC_UD_SHADER_START = 11,
   AC_UD_VS_VERTEX_BUFFERS = AC_UD_SHADER_START,
   AC_UD_VS_BASE_VERTEX_START_INSTANCE,
   AC_UD_VS_PROLOG_INPUTS,
   AC_UD_VS_MAX_UD,
   AC_UD_PS_EPILOG_PC,
   AC_UD_PS_MAX_UD,
   AC_UD_CS_GRID_SIZE = AC_UD_SHADER_START,
   AC_UD_CS_SBT_DESCRIPTORS,
   AC_UD_CS_RAY_LAUNCH_SIZE_ADDR,
   AC_UD_CS_TASK_RING_OFFSETS,
   AC_UD_CS_TASK_DRAW_ID,
   AC_UD_CS_TASK_IB,
   AC_UD_CS_MAX_UD,
   AC_UD_GS_MAX_UD,
   AC_UD_TCS_OFFCHIP_LAYOUT = AC_UD_VS_MAX_UD,
   AC_UD_TCS_MAX_UD,
   AC_UD_TES_NUM_PATCHES = AC_UD_SHADER_START,
   AC_UD_TES_MAX_UD,
   AC_UD_MAX_UD = AC_UD_CS_MAX_UD,
};

struct radv_stream_output {
   uint8_t location;
   uint8_t buffer;
   uint16_t offset;
   uint8_t component_mask;
   uint8_t stream;
};

struct radv_streamout_info {
   uint16_t num_outputs;
   struct radv_stream_output outputs[MAX_SO_OUTPUTS];
   uint16_t strides[MAX_SO_BUFFERS];
   uint32_t enabled_stream_buffers_mask;
};

struct radv_userdata_info {
   int8_t sgpr_idx;
   uint8_t num_sgprs;
};

struct radv_userdata_locations {
   struct radv_userdata_info descriptor_sets[MAX_SETS];
   struct radv_userdata_info shader_data[AC_UD_MAX_UD];
   uint32_t descriptor_sets_enabled;
};

struct radv_vs_output_info {
   uint8_t vs_output_param_offset[VARYING_SLOT_MAX];
   uint8_t clip_dist_mask;
   uint8_t cull_dist_mask;
   uint8_t param_exports;
   uint8_t prim_param_exports;
   bool writes_pointsize;
   bool writes_layer;
   bool writes_layer_per_primitive;
   bool writes_viewport_index;
   bool writes_viewport_index_per_primitive;
   bool writes_primitive_shading_rate;
   bool writes_primitive_shading_rate_per_primitive;
   bool export_prim_id;
   bool export_clip_dists;
   unsigned pos_exports;
};

struct gfx9_gs_info {
   uint32_t vgt_gs_onchip_cntl;
   uint32_t vgt_gs_max_prims_per_subgroup;
   uint32_t vgt_esgs_ring_itemsize;
   uint32_t lds_size;
};

struct gfx10_ngg_info {
   uint16_t ngg_emit_size; /* in dwords */
   uint32_t hw_max_esverts;
   uint32_t max_gsprims;
   uint32_t max_out_verts;
   uint32_t prim_amp_factor;
   uint32_t vgt_esgs_ring_itemsize;
   uint32_t esgs_ring_size;
   bool max_vert_out_per_gs_instance;
   bool enable_vertex_grouping;
};

struct radv_shader_info {
   uint64_t inline_push_constant_mask;
   bool can_inline_all_push_constants;
   bool loads_push_constants;
   bool loads_dynamic_offsets;
   uint32_t desc_set_used_mask;
   bool uses_view_index;
   bool uses_invocation_id;
   bool uses_prim_id;
   uint8_t wave_size;
   uint8_t ballot_bit_size;
   struct radv_userdata_locations user_sgprs_locs;
   bool is_ngg;
   bool is_ngg_passthrough;
   bool has_ngg_culling;
   bool has_ngg_early_prim_export;
   uint32_t num_lds_blocks_when_not_culling;
   uint32_t num_tess_patches;
   uint32_t esgs_itemsize; /* Only for VS or TES as ES */
   struct radv_vs_output_info outinfo;
   unsigned workgroup_size;
   bool force_vrs_per_vertex;
   struct {
      uint8_t input_usage_mask[RADV_VERT_ATTRIB_MAX];
      uint8_t output_usage_mask[VARYING_SLOT_VAR31 + 1];
      bool needs_draw_id;
      bool needs_instance_id;
      bool as_es;
      bool as_ls;
      bool tcs_in_out_eq;
      uint64_t tcs_temp_only_input_mask;
      uint8_t num_linked_outputs;
      bool needs_base_instance;
      bool use_per_attribute_vb_descs;
      uint32_t vb_desc_usage_mask;
      uint32_t input_slot_usage_mask;
      bool has_prolog;
      bool dynamic_inputs;
   } vs;
   struct {
      uint8_t output_usage_mask[VARYING_SLOT_VAR31 + 1];
      uint8_t num_stream_output_components[4];
      uint8_t output_streams[VARYING_SLOT_VAR31 + 1];
      uint8_t max_stream;
      unsigned gsvs_vertex_size;
      unsigned max_gsvs_emit_size;
      unsigned vertices_in;
      unsigned vertices_out;
      unsigned input_prim;
      unsigned output_prim;
      unsigned invocations;
      unsigned es_type; /* GFX9: VS or TES */
      uint8_t num_linked_inputs;
   } gs;
   struct {
      uint8_t output_usage_mask[VARYING_SLOT_VAR31 + 1];
      bool as_es;
      enum tess_primitive_mode _primitive_mode;
      enum gl_tess_spacing spacing;
      bool ccw;
      bool point_mode;
      uint8_t num_linked_inputs;
      uint8_t num_linked_patch_inputs;
      uint8_t num_linked_outputs;
   } tes;
   struct {
      bool uses_sample_shading;
      bool needs_sample_positions;
      bool writes_memory;
      bool writes_z;
      bool writes_stencil;
      bool writes_sample_mask;
      bool has_pcoord;
      bool prim_id_input;
      bool layer_input;
      bool viewport_index_input;
      uint8_t num_input_clips_culls;
      uint32_t input_mask;
      uint32_t input_per_primitive_mask;
      uint32_t flat_shaded_mask;
      uint32_t explicit_shaded_mask;
      uint32_t float16_shaded_mask;
      uint32_t num_interp;
      uint32_t num_prim_interp;
      bool can_discard;
      bool early_fragment_test;
      bool post_depth_coverage;
      bool reads_sample_mask_in;
      bool reads_front_face;
      bool reads_sample_id;
      bool reads_frag_shading_rate;
      bool reads_barycentric_model;
      bool reads_persp_sample;
      bool reads_persp_center;
      bool reads_persp_centroid;
      bool reads_linear_sample;
      bool reads_linear_center;
      bool reads_linear_centroid;
      uint8_t reads_frag_coord_mask;
      uint8_t reads_sample_pos_mask;
      uint8_t depth_layout;
      bool allow_flat_shading;
      bool has_epilog;
      unsigned spi_ps_input;
      unsigned colors_written;
   } ps;
   struct {
      bool uses_grid_size;
      bool uses_block_id[3];
      bool uses_thread_id[3];
      bool uses_local_invocation_idx;
      unsigned block_size[3];

      uint8_t subgroup_size;

      bool uses_sbt;
      bool uses_ray_launch_size;
   } cs;
   struct {
      uint64_t tes_inputs_read;
      uint64_t tes_patch_inputs_read;
      unsigned tcs_vertices_out;
      uint32_t num_lds_blocks;
      uint8_t num_linked_inputs;
      uint8_t num_linked_outputs;
      uint8_t num_linked_patch_outputs;
      bool tes_reads_tess_factors : 1;
   } tcs;
   struct {
      enum shader_prim output_prim;
      bool needs_ms_scratch_ring;
      bool has_task; /* If mesh shader is used together with a task shader. */
   } ms;

   struct radv_streamout_info so;

   struct gfx9_gs_info gs_ring_info;
   struct gfx10_ngg_info ngg_info;
};

struct radv_vs_input_state {
   uint32_t attribute_mask;

   uint32_t instance_rate_inputs;
   uint32_t nontrivial_divisors;
   uint32_t zero_divisors;
   uint32_t post_shuffle;
   /* Having two separate fields instead of a single uint64_t makes it easier to remove attributes
    * using bitwise arithmetic.
    */
   uint32_t alpha_adjust_lo;
   uint32_t alpha_adjust_hi;
   uint32_t nontrivial_formats;

   uint8_t bindings[MAX_VERTEX_ATTRIBS];
   uint32_t divisors[MAX_VERTEX_ATTRIBS];
   uint32_t offsets[MAX_VERTEX_ATTRIBS];
   uint8_t formats[MAX_VERTEX_ATTRIBS];
   uint8_t format_align_req_minus_1[MAX_VERTEX_ATTRIBS];
   uint8_t format_sizes[MAX_VERTEX_ATTRIBS];

   bool bindings_match_attrib;
};

struct radv_vs_prolog_key {
   const struct radv_vs_input_state *state;
   unsigned num_attributes;
   uint32_t misaligned_mask;
   bool as_ls;
   bool is_ngg;
   bool wave32;
   gl_shader_stage next_stage;
};

struct radv_ps_epilog_key {
   uint32_t spi_shader_col_format;

   /* Bitmasks, each bit represents one of the 8 MRTs. */
   uint8_t color_is_int8;
   uint8_t color_is_int10;
   uint8_t enable_mrt_output_nan_fixup;

   bool wave32;
};

enum radv_shader_binary_type { RADV_BINARY_TYPE_LEGACY, RADV_BINARY_TYPE_RTLD };

struct radv_shader_binary {
   enum radv_shader_binary_type type;
   gl_shader_stage stage;
   bool is_gs_copy_shader;

   struct ac_shader_config config;
   struct radv_shader_info info;

   /* Self-referential size so we avoid consistency issues. */
   uint32_t total_size;
};

struct radv_shader_binary_legacy {
   struct radv_shader_binary base;
   unsigned code_size;
   unsigned exec_size;
   unsigned ir_size;
   unsigned disasm_size;
   unsigned stats_size;

   /* data has size of stats_size + code_size + ir_size + disasm_size + 2,
    * where the +2 is for 0 of the ir strings. */
   uint8_t data[0];
};

struct radv_shader_binary_rtld {
   struct radv_shader_binary base;
   unsigned elf_size;
   unsigned llvm_ir_size;
   uint8_t data[0];
};

struct radv_shader_part_binary {
   uint8_t num_sgprs;
   uint8_t num_vgprs;
   uint8_t num_preserved_sgprs;
   unsigned code_size;
   unsigned disasm_size;
   uint8_t data[0];
};

struct radv_shader_arena {
   struct list_head list;
   struct list_head entries;
   struct radeon_winsys_bo *bo;
   char *ptr;
};

union radv_shader_arena_block {
   struct list_head pool;
   struct {
      /* List of blocks in the arena, sorted by address. */
      struct list_head list;
      /* For holes, a list_head for the free-list. For allocations, freelist.prev=NULL and
       * freelist.next is a pointer associated with the allocation.
       */
      struct list_head freelist;
      struct radv_shader_arena *arena;
      uint32_t offset;
      uint32_t size;
   };
};

struct radv_shader {
   uint32_t ref_count;

   uint64_t va;

   struct ac_shader_config config;
   uint8_t *code_ptr;
   uint32_t code_size;
   uint32_t exec_size;
   struct radv_shader_info info;
   struct radv_shader_binary *binary;

   /* debug only */
   char *spirv;
   uint32_t spirv_size;
   char *nir_string;
   char *disasm_string;
   char *ir_string;
   uint32_t *statistics;
};

struct radv_trap_handler_shader {
   struct radeon_winsys_bo *bo;
   union radv_shader_arena_block *alloc;
};

struct radv_shader_part {
   uint32_t ref_count;

   uint64_t va;

   struct radeon_winsys_bo *bo;
   union radv_shader_arena_block *alloc;
   uint32_t code_size;
   uint32_t rsrc1;
   uint8_t num_preserved_sgprs;
   bool nontrivial_divisors;

   struct radv_shader_part_binary *binary;

   /* debug only */
   char *disasm_string;
};

struct radv_pipeline_layout;

void radv_optimize_nir(struct nir_shader *shader, bool optimize_conservatively, bool allow_copies);
void radv_optimize_nir_algebraic(nir_shader *shader, bool opt_offsets);
bool radv_nir_lower_ycbcr_textures(nir_shader *shader, const struct radv_pipeline_layout *layout);

bool radv_nir_lower_ray_queries(nir_shader *shader, struct radv_device *device);

void radv_nir_apply_pipeline_layout(nir_shader *shader, struct radv_device *device,
                                    const struct radv_pipeline_layout *layout,
                                    const struct radv_shader_info *info,
                                    const struct radv_shader_args *args);

struct radv_pipeline_stage;

nir_shader *radv_shader_spirv_to_nir(struct radv_device *device,
                                     const struct radv_pipeline_stage *stage,
                                     const struct radv_pipeline_key *key);

void radv_nir_lower_abi(nir_shader *shader, enum amd_gfx_level gfx_level,
                        const struct radv_shader_info *info, const struct radv_shader_args *args,
                        const struct radv_pipeline_key *pl_key, bool use_llvm);

void radv_init_shader_arenas(struct radv_device *device);
void radv_destroy_shader_arenas(struct radv_device *device);

struct radv_pipeline_shader_stack_size;

VkResult radv_create_shaders(struct radv_pipeline *pipeline,
                             struct radv_pipeline_layout *pipeline_layout,
                             struct radv_device *device, struct radv_pipeline_cache *cache,
                             const struct radv_pipeline_key *key,
                             const VkPipelineShaderStageCreateInfo *pStages,
                             uint32_t stageCount,
                             const VkPipelineCreateFlags flags, const uint8_t *custom_hash,
                             const VkPipelineCreationFeedbackCreateInfo *creation_feedback,
                             struct radv_pipeline_shader_stack_size **stack_sizes,
                             uint32_t *num_stack_sizes,
                             gl_shader_stage *last_vgt_api_stage);

struct radv_shader_args;

struct radv_shader *radv_shader_create(struct radv_device *device,
                                       struct radv_shader_binary *binary,
                                       bool keep_shader_info, bool from_cache,
                                       const struct radv_shader_args *args);
struct radv_shader *radv_shader_nir_to_asm(
   struct radv_device *device, struct radv_pipeline_stage *stage, struct nir_shader *const *shaders,
   int shader_count, const struct radv_pipeline_key *key, bool keep_shader_info, bool keep_statistic_info);

bool radv_shader_binary_upload(struct radv_device *device, const struct radv_shader_binary *binary,
                               struct radv_shader *shader, void *dest_ptr);

void radv_shader_part_binary_upload(const struct radv_shader_part_binary *binary, void *dest_ptr);

union radv_shader_arena_block *radv_alloc_shader_memory(struct radv_device *device, uint32_t size,
                                                        void *ptr);
void radv_free_shader_memory(struct radv_device *device, union radv_shader_arena_block *alloc);

struct radv_shader *
radv_create_gs_copy_shader(struct radv_device *device, struct nir_shader *nir,
                           const struct radv_shader_info *info, const struct radv_shader_args *args,
                           bool keep_shader_info, bool keep_statistic_info,
                           bool disable_optimizations);

struct radv_trap_handler_shader *radv_create_trap_handler_shader(struct radv_device *device);
uint64_t radv_trap_handler_shader_get_va(const struct radv_trap_handler_shader *trap);
void radv_trap_handler_shader_destroy(struct radv_device *device,
                                      struct radv_trap_handler_shader *trap);

struct radv_shader_part *radv_create_vs_prolog(struct radv_device *device,
                                               const struct radv_vs_prolog_key *key);

struct radv_shader_part *radv_create_ps_epilog(struct radv_device *device,
                                               const struct radv_ps_epilog_key *key);

void radv_shader_destroy(struct radv_device *device, struct radv_shader *shader);

void radv_shader_part_destroy(struct radv_device *device, struct radv_shader_part *shader_part);

uint64_t radv_shader_get_va(const struct radv_shader *shader);
struct radv_shader *radv_find_shader(struct radv_device *device, uint64_t pc);

unsigned radv_get_max_waves(const struct radv_device *device, struct radv_shader *shader,
                            gl_shader_stage stage);

const char *radv_get_shader_name(const struct radv_shader_info *info, gl_shader_stage stage);

unsigned radv_compute_spi_ps_input(const struct radv_pipeline_key *pipeline_key,
                                   const struct radv_shader_info *info);

bool radv_can_dump_shader(struct radv_device *device, nir_shader *nir, bool meta_shader);

bool radv_can_dump_shader_stats(struct radv_device *device, nir_shader *nir);

VkResult radv_dump_shader_stats(struct radv_device *device, struct radv_pipeline *pipeline,
                                gl_shader_stage stage, FILE *output);

static inline struct radv_shader *
radv_shader_ref(struct radv_shader *shader)
{
   assert(shader && shader->ref_count >= 1);
   p_atomic_inc(&shader->ref_count);
   return shader;
}

static inline void
radv_shader_unref(struct radv_device *device, struct radv_shader *shader)
{
   assert(shader && shader->ref_count >= 1);
   if (p_atomic_dec_zero(&shader->ref_count))
      radv_shader_destroy(device, shader);
}

static inline struct radv_shader_part *
radv_shader_part_ref(struct radv_shader_part *shader_part)
{
   assert(shader_part && shader_part->ref_count >= 1);
   p_atomic_inc(&shader_part->ref_count);
   return shader_part;
}

static inline void
radv_shader_part_unref(struct radv_device *device, struct radv_shader_part *shader_part)
{
   assert(shader_part && shader_part->ref_count >= 1);
   if (p_atomic_dec_zero(&shader_part->ref_count))
      radv_shader_part_destroy(device, shader_part);
}

static inline unsigned
calculate_tess_lds_size(enum amd_gfx_level gfx_level, unsigned tcs_num_input_vertices,
                        unsigned tcs_num_output_vertices, unsigned tcs_num_inputs,
                        unsigned tcs_num_patches, unsigned tcs_num_outputs,
                        unsigned tcs_num_patch_outputs)
{
   unsigned input_vertex_size = tcs_num_inputs * 16;
   unsigned output_vertex_size = tcs_num_outputs * 16;

   unsigned input_patch_size = tcs_num_input_vertices * input_vertex_size;

   unsigned pervertex_output_patch_size = tcs_num_output_vertices * output_vertex_size;
   unsigned output_patch_size = pervertex_output_patch_size + tcs_num_patch_outputs * 16;

   unsigned output_patch0_offset = input_patch_size * tcs_num_patches;

   unsigned lds_size = output_patch0_offset + output_patch_size * tcs_num_patches;

   if (gfx_level >= GFX7) {
      assert(lds_size <= 65536);
      lds_size = align(lds_size, 512) / 512;
   } else {
      assert(lds_size <= 32768);
      lds_size = align(lds_size, 256) / 256;
   }

   return lds_size;
}

static inline unsigned
get_tcs_num_patches(unsigned tcs_num_input_vertices, unsigned tcs_num_output_vertices,
                    unsigned tcs_num_inputs, unsigned tcs_num_outputs,
                    unsigned tcs_num_patch_outputs, unsigned tess_offchip_block_dw_size,
                    enum amd_gfx_level gfx_level, enum radeon_family family)
{
   uint32_t input_vertex_size = tcs_num_inputs * 16;
   uint32_t input_patch_size = tcs_num_input_vertices * input_vertex_size;
   uint32_t output_vertex_size = tcs_num_outputs * 16;
   uint32_t pervertex_output_patch_size = tcs_num_output_vertices * output_vertex_size;
   uint32_t output_patch_size = pervertex_output_patch_size + tcs_num_patch_outputs * 16;

   /* Ensure that we only need one wave per SIMD so we don't need to check
    * resource usage. Also ensures that the number of tcs in and out
    * vertices per threadgroup are at most 256.
    */
   unsigned num_patches = 64 / MAX2(tcs_num_input_vertices, tcs_num_output_vertices) * 4;
   /* Make sure that the data fits in LDS. This assumes the shaders only
    * use LDS for the inputs and outputs.
    */
   unsigned hardware_lds_size = 32768;

   /* Looks like STONEY hangs if we use more than 32 KiB LDS in a single
    * threadgroup, even though there is more than 32 KiB LDS.
    *
    * Test: dEQP-VK.tessellation.shader_input_output.barrier
    */
   if (gfx_level >= GFX7 && family != CHIP_STONEY)
      hardware_lds_size = 65536;

   if (input_patch_size + output_patch_size)
      num_patches = MIN2(num_patches, hardware_lds_size / (input_patch_size + output_patch_size));
   /* Make sure the output data fits in the offchip buffer */
   if (output_patch_size)
      num_patches = MIN2(num_patches, (tess_offchip_block_dw_size * 4) / output_patch_size);
   /* Not necessary for correctness, but improves performance. The
    * specific value is taken from the proprietary driver.
    */
   num_patches = MIN2(num_patches, 40);

   /* GFX6 bug workaround - limit LS-HS threadgroups to only one wave. */
   if (gfx_level == GFX6) {
      unsigned one_wave = 64 / MAX2(tcs_num_input_vertices, tcs_num_output_vertices);
      num_patches = MIN2(num_patches, one_wave);
   }
   return num_patches;
}

void radv_lower_io(struct radv_device *device, nir_shader *nir);

bool radv_lower_io_to_mem(struct radv_device *device, struct radv_pipeline_stage *stage);

bool radv_lower_view_index(nir_shader *nir, bool per_primitive);

void radv_lower_ngg(struct radv_device *device, struct radv_pipeline_stage *ngg_stage,
                    const struct radv_pipeline_key *pl_key);

bool radv_consider_culling(const struct radv_physical_device *pdevice, struct nir_shader *nir,
                           uint64_t ps_inputs_read, unsigned num_vertices_per_primitive,
                           const struct radv_shader_info *info);

void radv_get_nir_options(struct radv_physical_device *device);

bool radv_force_primitive_shading_rate(nir_shader *nir, struct radv_device *device);

bool radv_lower_fs_intrinsics(nir_shader *nir, const struct radv_pipeline_stage *fs_stage,
                              const struct radv_pipeline_key *key);

#endif
