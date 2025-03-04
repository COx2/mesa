/*
 * Copyright © 2017 Red Hat
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
#include "nir/nir.h"
#include "nir/nir_xfb_info.h"
#include "radv_private.h"
#include "radv_shader.h"

#include "ac_nir.h"

static void
mark_sampler_desc(const nir_variable *var, struct radv_shader_info *info)
{
   info->desc_set_used_mask |= (1u << var->data.descriptor_set);
}

static void
gather_intrinsic_load_input_info(const nir_shader *nir, const nir_intrinsic_instr *instr,
                                 struct radv_shader_info *info)
{
   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX: {
      unsigned idx = nir_intrinsic_io_semantics(instr).location;
      unsigned component = nir_intrinsic_component(instr);
      unsigned mask = nir_ssa_def_components_read(&instr->dest.ssa);
      mask = (instr->dest.ssa.bit_size == 64 ? util_widen_mask(mask, 2) : mask) << component;

      info->vs.input_usage_mask[idx] |= mask & 0xf;
      if (mask >> 4)
         info->vs.input_usage_mask[idx + 1] |= mask >> 4;
      break;
   }
   default:
      break;
   }
}

static void
gather_intrinsic_store_output_info(const nir_shader *nir, const nir_intrinsic_instr *instr,
                                   struct radv_shader_info *info)
{
   unsigned idx = nir_intrinsic_base(instr);
   unsigned num_slots = nir_intrinsic_io_semantics(instr).num_slots;
   unsigned component = nir_intrinsic_component(instr);
   unsigned write_mask = nir_intrinsic_write_mask(instr);
   uint8_t *output_usage_mask = NULL;

   if (instr->src[0].ssa->bit_size == 64)
      write_mask = util_widen_mask(write_mask, 2);

   switch (nir->info.stage) {
   case MESA_SHADER_VERTEX:
      output_usage_mask = info->vs.output_usage_mask;
      break;
   case MESA_SHADER_TESS_EVAL:
      output_usage_mask = info->tes.output_usage_mask;
      break;
   case MESA_SHADER_GEOMETRY:
      output_usage_mask = info->gs.output_usage_mask;
      break;
   case MESA_SHADER_FRAGMENT:
      if (idx >= FRAG_RESULT_DATA0)
         info->ps.colors_written |= 0xf << (4 * (idx - FRAG_RESULT_DATA0));
      break;
   default:
      break;
   }

   if (output_usage_mask) {
      for (unsigned i = 0; i < num_slots; i++) {
         output_usage_mask[idx + i] |= ((write_mask >> (i * 4)) & 0xf) << component;
      }
   }
}

static void
gather_push_constant_info(const nir_shader *nir, const nir_intrinsic_instr *instr,
                          struct radv_shader_info *info)
{
   info->loads_push_constants = true;

   if (nir_src_is_const(instr->src[0]) && instr->dest.ssa.bit_size >= 32) {
      uint32_t start = (nir_intrinsic_base(instr) + nir_src_as_uint(instr->src[0])) / 4u;
      uint32_t size = instr->num_components * (instr->dest.ssa.bit_size / 32u);

      if (start + size <= (MAX_PUSH_CONSTANTS_SIZE / 4u)) {
         info->inline_push_constant_mask |= u_bit_consecutive64(start, size);
         return;
      }
   }

   info->can_inline_all_push_constants = false;
}

static void
gather_intrinsic_info(const nir_shader *nir, const nir_intrinsic_instr *instr,
                      struct radv_shader_info *info)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_at_offset: {
      enum glsl_interp_mode mode = nir_intrinsic_interp_mode(instr);
      switch (mode) {
      case INTERP_MODE_SMOOTH:
      case INTERP_MODE_NONE:
         if (instr->intrinsic == nir_intrinsic_load_barycentric_pixel ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_sample ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_offset)
            info->ps.reads_persp_center = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_centroid)
            info->ps.reads_persp_centroid = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_sample)
            info->ps.reads_persp_sample = true;
         break;
      case INTERP_MODE_NOPERSPECTIVE:
         if (instr->intrinsic == nir_intrinsic_load_barycentric_pixel ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_sample ||
             instr->intrinsic == nir_intrinsic_load_barycentric_at_offset)
            info->ps.reads_linear_center = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_centroid)
            info->ps.reads_linear_centroid = true;
         else if (instr->intrinsic == nir_intrinsic_load_barycentric_sample)
            info->ps.reads_linear_sample = true;
         break;
      default:
         break;
      }
      if (instr->intrinsic == nir_intrinsic_load_barycentric_at_sample)
         info->ps.needs_sample_positions = true;
      break;
   }
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_workgroup_id: {
      unsigned mask = nir_ssa_def_components_read(&instr->dest.ssa);
      while (mask) {
         unsigned i = u_bit_scan(&mask);

         if (instr->intrinsic == nir_intrinsic_load_workgroup_id)
            info->cs.uses_block_id[i] = true;
         else
            info->cs.uses_thread_id[i] = true;
      }
      break;
   }
   case nir_intrinsic_load_frag_coord:
      info->ps.reads_frag_coord_mask |= nir_ssa_def_components_read(&instr->dest.ssa);
      break;
   case nir_intrinsic_load_sample_pos:
      info->ps.reads_sample_pos_mask |= nir_ssa_def_components_read(&instr->dest.ssa);
      break;
   case nir_intrinsic_load_push_constant:
      gather_push_constant_info(nir, instr, info);
      break;
   case nir_intrinsic_vulkan_resource_index:
      info->desc_set_used_mask |= (1u << nir_intrinsic_desc_set(instr));
      break;
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_sparse_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic_add:
   case nir_intrinsic_image_deref_atomic_imin:
   case nir_intrinsic_image_deref_atomic_umin:
   case nir_intrinsic_image_deref_atomic_imax:
   case nir_intrinsic_image_deref_atomic_umax:
   case nir_intrinsic_image_deref_atomic_and:
   case nir_intrinsic_image_deref_atomic_or:
   case nir_intrinsic_image_deref_atomic_xor:
   case nir_intrinsic_image_deref_atomic_exchange:
   case nir_intrinsic_image_deref_atomic_comp_swap:
   case nir_intrinsic_image_deref_atomic_fmin:
   case nir_intrinsic_image_deref_atomic_fmax:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples: {
      nir_variable *var =
         nir_deref_instr_get_variable(nir_instr_as_deref(instr->src[0].ssa->parent_instr));
      mark_sampler_desc(var, info);
      break;
   }
   case nir_intrinsic_load_input:
      gather_intrinsic_load_input_info(nir, instr, info);
      break;
   case nir_intrinsic_store_output:
      gather_intrinsic_store_output_info(nir, instr, info);
      break;
   case nir_intrinsic_load_sbt_base_amd:
      info->cs.uses_sbt = true;
      break;
   case nir_intrinsic_load_force_vrs_rates_amd:
      info->force_vrs_per_vertex = true;
      break;
   default:
      break;
   }
}

static void
gather_tex_info(const nir_shader *nir, const nir_tex_instr *instr, struct radv_shader_info *info)
{
   for (unsigned i = 0; i < instr->num_srcs; i++) {
      switch (instr->src[i].src_type) {
      case nir_tex_src_texture_deref:
         mark_sampler_desc(nir_deref_instr_get_variable(nir_src_as_deref(instr->src[i].src)), info);
         break;
      case nir_tex_src_sampler_deref:
         mark_sampler_desc(nir_deref_instr_get_variable(nir_src_as_deref(instr->src[i].src)), info);
         break;
      default:
         break;
      }
   }
}

static void
gather_info_block(const nir_shader *nir, const nir_block *block, struct radv_shader_info *info)
{
   nir_foreach_instr (instr, block) {
      switch (instr->type) {
      case nir_instr_type_intrinsic:
         gather_intrinsic_info(nir, nir_instr_as_intrinsic(instr), info);
         break;
      case nir_instr_type_tex:
         gather_tex_info(nir, nir_instr_as_tex(instr), info);
         break;
      default:
         break;
      }
   }
}

static void
mark_16bit_ps_input(struct radv_shader_info *info, const struct glsl_type *type, int location)
{
   if (glsl_type_is_scalar(type) || glsl_type_is_vector(type) || glsl_type_is_matrix(type)) {
      unsigned attrib_count = glsl_count_attribute_slots(type, false);
      if (glsl_type_is_16bit(type)) {
         info->ps.float16_shaded_mask |= ((1ull << attrib_count) - 1) << location;
      }
   } else if (glsl_type_is_array(type)) {
      unsigned stride = glsl_count_attribute_slots(glsl_get_array_element(type), false);
      for (unsigned i = 0; i < glsl_get_length(type); ++i) {
         mark_16bit_ps_input(info, glsl_get_array_element(type), location + i * stride);
      }
   } else {
      assert(glsl_type_is_struct_or_ifc(type));
      for (unsigned i = 0; i < glsl_get_length(type); i++) {
         mark_16bit_ps_input(info, glsl_get_struct_field(type, i), location);
         location += glsl_count_attribute_slots(glsl_get_struct_field(type, i), false);
      }
   }
}

static void
gather_xfb_info(const nir_shader *nir, struct radv_shader_info *info)
{
   struct radv_streamout_info *so = &info->so;

   if (!nir->xfb_info)
      return;

   const nir_xfb_info *xfb = nir->xfb_info;
   assert(xfb->output_count <= MAX_SO_OUTPUTS);
   so->num_outputs = xfb->output_count;

   for (unsigned i = 0; i < xfb->output_count; i++) {
      struct radv_stream_output *output = &so->outputs[i];

      output->buffer = xfb->outputs[i].buffer;
      output->stream = xfb->buffer_to_stream[xfb->outputs[i].buffer];
      output->offset = xfb->outputs[i].offset;
      output->location = xfb->outputs[i].location;
      output->component_mask = xfb->outputs[i].component_mask;

      so->enabled_stream_buffers_mask |= (1 << output->buffer) << (output->stream * 4);
   }

   for (unsigned i = 0; i < NIR_MAX_XFB_BUFFERS; i++) {
      so->strides[i] = xfb->buffers[i].stride / 4;
   }
}

static void
assign_outinfo_param(struct radv_vs_output_info *outinfo, gl_varying_slot idx,
                     unsigned *total_param_exports)
{
   if (outinfo->vs_output_param_offset[idx] == AC_EXP_PARAM_UNDEFINED)
      outinfo->vs_output_param_offset[idx] = (*total_param_exports)++;
}

static void
assign_outinfo_params(struct radv_vs_output_info *outinfo, uint64_t mask,
                      unsigned *total_param_exports)
{
   u_foreach_bit64(idx, mask) {
      if (idx >= VARYING_SLOT_VAR0 || idx == VARYING_SLOT_LAYER ||
          idx == VARYING_SLOT_PRIMITIVE_ID || idx == VARYING_SLOT_VIEWPORT)
         assign_outinfo_param(outinfo, idx, total_param_exports);
   }
}

static uint8_t
radv_get_wave_size(struct radv_device *device,  gl_shader_stage stage,
                   const struct radv_shader_info *info)
{
   if (stage == MESA_SHADER_GEOMETRY && !info->is_ngg)
      return 64;
   else if (stage == MESA_SHADER_COMPUTE) {
      return info->cs.subgroup_size;
   } else if (stage == MESA_SHADER_FRAGMENT)
      return device->physical_device->ps_wave_size;
   else if (stage == MESA_SHADER_TASK)
      return device->physical_device->cs_wave_size;
   else
      return device->physical_device->ge_wave_size;
}

static uint8_t
radv_get_ballot_bit_size(struct radv_device *device, gl_shader_stage stage,
                         const struct radv_shader_info *info)
{
   if (stage == MESA_SHADER_COMPUTE && info->cs.subgroup_size)
      return info->cs.subgroup_size;
   return 64;
}

static void
gather_info_input_decl_vs(const nir_shader *nir, unsigned location, const struct glsl_type *type,
                          const struct radv_pipeline_key *key, struct radv_shader_info *info)
{
   if (glsl_type_is_scalar(type) || glsl_type_is_vector(type)) {
      if (key->vs.instance_rate_inputs & BITFIELD_BIT(location)) {
         info->vs.needs_instance_id = true;
         info->vs.needs_base_instance = true;
      }

      if (info->vs.use_per_attribute_vb_descs)
         info->vs.vb_desc_usage_mask |= BITFIELD_BIT(location);
      else
         info->vs.vb_desc_usage_mask |= BITFIELD_BIT(key->vs.vertex_attribute_bindings[location]);

      info->vs.input_slot_usage_mask |=
         BITFIELD_RANGE(location, glsl_count_attribute_slots(type, false));
   } else if (glsl_type_is_matrix(type) || glsl_type_is_array(type)) {
      const struct glsl_type *elem = glsl_get_array_element(type);
      unsigned stride = glsl_count_attribute_slots(elem, false);

      for (unsigned i = 0; i < glsl_get_length(type); ++i)
         gather_info_input_decl_vs(nir, location + i * stride, elem, key, info);
   } else {
      assert(glsl_type_is_struct_or_ifc(type));

      for (unsigned i = 0; i < glsl_get_length(type); i++) {
         const struct glsl_type *field = glsl_get_struct_field(type, i);
         gather_info_input_decl_vs(nir, location, field, key, info);
         location += glsl_count_attribute_slots(field, false);
      }
   }
}

static void
gather_shader_info_vs(struct radv_device *device, const nir_shader *nir,
                      const struct radv_pipeline_key *pipeline_key, struct radv_shader_info *info)
{
   if (pipeline_key->vs.dynamic_input_state && nir->info.inputs_read) {
      info->vs.has_prolog = true;
      info->vs.dynamic_inputs = true;
   }

   /* Use per-attribute vertex descriptors to prevent faults and for correct bounds checking. */
   info->vs.use_per_attribute_vb_descs = device->robust_buffer_access || info->vs.dynamic_inputs;

   /* We have to ensure consistent input register assignments between the main shader and the
    * prolog.
    */
   info->vs.needs_instance_id |= info->vs.has_prolog;
   info->vs.needs_base_instance |= info->vs.has_prolog;
   info->vs.needs_draw_id |= info->vs.has_prolog;

   nir_foreach_shader_in_variable(var, nir)
      gather_info_input_decl_vs(nir, var->data.location - VERT_ATTRIB_GENERIC0, var->type,
                                pipeline_key, info);
}

static void
gather_shader_info_tcs(struct radv_device *device, const nir_shader *nir,
                       const struct radv_pipeline_key *pipeline_key, struct radv_shader_info *info)
{
   info->tcs.tcs_vertices_out = nir->info.tess.tcs_vertices_out;

   if (!(pipeline_key->dynamic_patch_control_points)) {
      /* Number of tessellation patches per workgroup processed by the current pipeline. */
      info->num_tess_patches =
         get_tcs_num_patches(pipeline_key->tcs.tess_input_vertices, nir->info.tess.tcs_vertices_out,
                             info->tcs.num_linked_inputs, info->tcs.num_linked_outputs,
                             info->tcs.num_linked_patch_outputs,
                             device->physical_device->hs.tess_offchip_block_dw_size,
                             device->physical_device->rad_info.gfx_level,
                             device->physical_device->rad_info.family);

      /* LDS size used by VS+TCS for storing TCS inputs and outputs. */
      info->tcs.num_lds_blocks =
         calculate_tess_lds_size(device->physical_device->rad_info.gfx_level,
                                 pipeline_key->tcs.tess_input_vertices,
                                 nir->info.tess.tcs_vertices_out, info->tcs.num_linked_inputs,
                                 info->num_tess_patches, info->tcs.num_linked_outputs,
                                 info->tcs.num_linked_patch_outputs);
   }
}

static void
gather_shader_info_tes(const nir_shader *nir, struct radv_shader_info *info)
{
   info->tes._primitive_mode = nir->info.tess._primitive_mode;
   info->tes.spacing = nir->info.tess.spacing;
   info->tes.ccw = nir->info.tess.ccw;
   info->tes.point_mode = nir->info.tess.point_mode;
}

static void
gather_shader_info_gs(const nir_shader *nir, struct radv_shader_info *info)
{
   unsigned add_clip = nir->info.clip_distance_array_size + nir->info.cull_distance_array_size > 4;
   info->gs.gsvs_vertex_size = (util_bitcount64(nir->info.outputs_written) + add_clip) * 16;
   info->gs.max_gsvs_emit_size = info->gs.gsvs_vertex_size * nir->info.gs.vertices_out;

   info->gs.vertices_in = nir->info.gs.vertices_in;
   info->gs.vertices_out = nir->info.gs.vertices_out;
   info->gs.input_prim = nir->info.gs.input_primitive;
   info->gs.output_prim = nir->info.gs.output_primitive;
   info->gs.invocations = nir->info.gs.invocations;
   info->gs.max_stream =
      nir->info.gs.active_stream_mask ? util_last_bit(nir->info.gs.active_stream_mask) - 1 : 0;

   nir_foreach_shader_out_variable(var, nir) {
      unsigned num_components = glsl_get_component_slots(var->type);
      unsigned stream = var->data.stream;
      unsigned idx = var->data.location;

      assert(stream < 4);

      info->gs.num_stream_output_components[stream] += num_components;
      info->gs.output_streams[idx] = stream;
   }
}

static void
gather_shader_info_mesh(const nir_shader *nir, struct radv_shader_info *info)
{
   struct gfx10_ngg_info *ngg_info = &info->ngg_info;

   info->ms.output_prim = nir->info.mesh.primitive_type;

   /* Special case for mesh shader workgroups.
    *
    * Mesh shaders don't have any real vertex input, but they can produce
    * an arbitrary number of vertices and primitives (up to 256).
    * We need to precisely control the number of mesh shader workgroups
    * that are launched from draw calls.
    *
    * To achieve that, we set:
    * - input primitive topology to point list
    * - input vertex and primitive count to 1
    * - max output vertex count and primitive amplification factor
    *   to the boundaries of the shader
    *
    * With that, in the draw call:
    * - drawing 1 input vertex ~ launching 1 mesh shader workgroup
    *
    * In the shader:
    * - base vertex ~ first workgroup index (firstTask in NV_mesh_shader)
    * - input vertex id ~ workgroup id (in 1D - shader needs to calculate in 3D)
    *
    * Notes:
    * - without GS_EN=1 PRIM_AMP_FACTOR and MAX_VERTS_PER_SUBGROUP don't seem to work
    * - with GS_EN=1 we must also set VGT_GS_MAX_VERT_OUT (otherwise the GPU hangs)
    * - with GS_FAST_LAUNCH=1 every lane's VGPRs are initialized to the same input vertex index
    *
    */
   ngg_info->enable_vertex_grouping = true;
   ngg_info->esgs_ring_size = 1;
   ngg_info->hw_max_esverts = 1;
   ngg_info->max_gsprims = 1;
   ngg_info->max_out_verts = nir->info.mesh.max_vertices_out;
   ngg_info->max_vert_out_per_gs_instance = false;
   ngg_info->ngg_emit_size = 0;
   ngg_info->prim_amp_factor = nir->info.mesh.max_primitives_out;
   ngg_info->vgt_esgs_ring_itemsize = 1;

   unsigned min_ngg_workgroup_size =
      ac_compute_ngg_workgroup_size(ngg_info->hw_max_esverts, ngg_info->max_gsprims,
                                    ngg_info->max_out_verts, ngg_info->prim_amp_factor);

   unsigned api_workgroup_size =
      ac_compute_cs_workgroup_size(nir->info.workgroup_size, false, UINT32_MAX);

   info->workgroup_size = MAX2(min_ngg_workgroup_size, api_workgroup_size);
}

static void
gather_shader_info_fs(const nir_shader *nir, const struct radv_pipeline_key *pipeline_key,
                      struct radv_shader_info *info)
{
   uint64_t per_primitive_input_mask = nir->info.inputs_read & nir->info.per_primitive_inputs;
   unsigned num_per_primitive_inputs = util_bitcount64(per_primitive_input_mask);
   assert(num_per_primitive_inputs <= nir->num_inputs);

   info->ps.num_interp = nir->num_inputs - num_per_primitive_inputs;
   info->ps.num_prim_interp = num_per_primitive_inputs;
   info->ps.can_discard = nir->info.fs.uses_discard;
   info->ps.early_fragment_test = nir->info.fs.early_fragment_tests;
   info->ps.post_depth_coverage = nir->info.fs.post_depth_coverage;
   info->ps.depth_layout = nir->info.fs.depth_layout;
   info->ps.uses_sample_shading = nir->info.fs.uses_sample_shading;
   info->ps.writes_memory = nir->info.writes_memory;
   info->ps.has_pcoord = nir->info.inputs_read & VARYING_BIT_PNTC;
   info->ps.prim_id_input = nir->info.inputs_read & VARYING_BIT_PRIMITIVE_ID;
   info->ps.layer_input = nir->info.inputs_read & VARYING_BIT_LAYER;
   info->ps.viewport_index_input = nir->info.inputs_read & VARYING_BIT_VIEWPORT;
   info->ps.writes_z = nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH);
   info->ps.writes_stencil = nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL);
   info->ps.writes_sample_mask = nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK);
   info->ps.reads_sample_mask_in = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN);
   info->ps.reads_sample_id = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID);
   info->ps.reads_frag_shading_rate = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_SHADING_RATE);
   info->ps.reads_front_face = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRONT_FACE);
   info->ps.reads_barycentric_model = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BARYCENTRIC_PULL_MODEL);

   bool uses_persp_or_linear_interp = info->ps.reads_persp_center ||
                                      info->ps.reads_persp_centroid ||
                                      info->ps.reads_persp_sample ||
                                      info->ps.reads_linear_center ||
                                      info->ps.reads_linear_centroid ||
                                      info->ps.reads_linear_sample;

   info->ps.allow_flat_shading =
      !(uses_persp_or_linear_interp || info->ps.needs_sample_positions ||
        info->ps.writes_memory || nir->info.fs.needs_quad_helper_invocations ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_POINT_COORD) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_ID) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_POS) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN) ||
        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_HELPER_INVOCATION));

   info->ps.spi_ps_input = radv_compute_spi_ps_input(pipeline_key, info);

   info->ps.has_epilog = pipeline_key->ps.has_epilog;

   nir_foreach_shader_in_variable(var, nir) {
      unsigned attrib_count = glsl_count_attribute_slots(var->type, false);
      int idx = var->data.location;

      switch (idx) {
      case VARYING_SLOT_CLIP_DIST0:
      case VARYING_SLOT_CLIP_DIST1:
         info->ps.num_input_clips_culls += attrib_count;
         break;
      default:
         break;
      }

      if (var->data.compact) {
         unsigned component_count = var->data.location_frac + glsl_get_length(var->type);
         attrib_count = (component_count + 3) / 4;
      } else {
         mark_16bit_ps_input(info, var->type, var->data.driver_location);
      }

      uint64_t mask = ((1ull << attrib_count) - 1);

      if (!var->data.per_primitive) {
         if (var->data.interpolation == INTERP_MODE_FLAT)
            info->ps.flat_shaded_mask |= mask << var->data.driver_location;
         else if (var->data.interpolation == INTERP_MODE_EXPLICIT)
            info->ps.explicit_shaded_mask |= mask << var->data.driver_location;
      }

      if (var->data.location >= VARYING_SLOT_VAR0) {
         if (var->data.per_primitive)
            info->ps.input_per_primitive_mask |= mask << (var->data.location - VARYING_SLOT_VAR0);
         else
            info->ps.input_mask |= mask << (var->data.location - VARYING_SLOT_VAR0);
      }
   }
}

static void
gather_shader_info_cs(struct radv_device *device, const nir_shader *nir,
                      const struct radv_pipeline_key *pipeline_key, struct radv_shader_info *info)
{
   info->cs.uses_ray_launch_size = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_RAY_LAUNCH_SIZE_ADDR_AMD);

   unsigned subgroup_size = pipeline_key->cs.compute_subgroup_size;
   unsigned req_subgroup_size = subgroup_size;
   bool require_full_subgroups = pipeline_key->cs.require_full_subgroups;

   if (!subgroup_size)
      subgroup_size = device->physical_device->cs_wave_size;

   unsigned local_size =
      nir->info.workgroup_size[0] * nir->info.workgroup_size[1] * nir->info.workgroup_size[2];

   /* Games don't always request full subgroups when they should, which can cause bugs if cswave32
    * is enabled.
    */
   if (device->physical_device->cs_wave_size == 32 && nir->info.cs.uses_wide_subgroup_intrinsics &&
       !req_subgroup_size && local_size % RADV_SUBGROUP_SIZE == 0)
      require_full_subgroups = true;

   if (require_full_subgroups && !req_subgroup_size) {
      /* don't use wave32 pretending to be wave64 */
      subgroup_size = RADV_SUBGROUP_SIZE;
   }

   info->cs.subgroup_size = subgroup_size;
}

static void
gather_shader_info_task(const nir_shader *nir, struct radv_shader_info *info)
{
   /* Task shaders always need these for the I/O lowering even if the API shader doesn't actually
    * use them.
    */

   /* Needed to address the IB to read firstTask in NV_mesh_shader. */
   info->vs.needs_draw_id |=
      BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_WORKGROUP_ID);

   /* Needed to address the task draw/payload rings. */
   info->cs.uses_block_id[0] = true;
   info->cs.uses_block_id[1] = true;
   info->cs.uses_block_id[2] = true;
   info->cs.uses_grid_size = true;

   /* Needed for storing draw ready only on the 1st thread. */
   info->cs.uses_local_invocation_idx = true;
}

void
radv_nir_shader_info_init(struct radv_shader_info *info)
{
   /* Assume that shaders can inline all push constants by default. */
   info->can_inline_all_push_constants = true;
}

void
radv_nir_shader_info_pass(struct radv_device *device, const struct nir_shader *nir,
                          const struct radv_pipeline_layout *layout,
                          const struct radv_pipeline_key *pipeline_key,
                          struct radv_shader_info *info)
{
   struct nir_function *func = (struct nir_function *)exec_list_get_head_const(&nir->functions);

   if (layout && layout->dynamic_offset_count &&
       (layout->dynamic_shader_stages & mesa_to_vk_shader_stage(nir->info.stage))) {
      info->loads_push_constants = true;
      info->loads_dynamic_offsets = true;
   }

   nir_foreach_block (block, func->impl) {
      gather_info_block(nir, block, info);
   }

   if (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL ||
       nir->info.stage == MESA_SHADER_GEOMETRY)
      gather_xfb_info(nir, info);

   if (nir->info.stage == MESA_SHADER_VERTEX || nir->info.stage == MESA_SHADER_TESS_EVAL ||
       nir->info.stage == MESA_SHADER_GEOMETRY || nir->info.stage == MESA_SHADER_MESH) {
      struct radv_vs_output_info *outinfo = &info->outinfo;

      /* These are not compiled into neither output param nor position exports. */
      uint64_t special_mask = BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_COUNT) |
                              BITFIELD64_BIT(VARYING_SLOT_PRIMITIVE_INDICES) |
                              BITFIELD64_BIT(VARYING_SLOT_CULL_PRIMITIVE);
      uint64_t per_prim_mask =
         nir->info.outputs_written & nir->info.per_primitive_outputs & ~special_mask;
      uint64_t per_vtx_mask =
         nir->info.outputs_written & ~nir->info.per_primitive_outputs & ~special_mask;

      /* Mesh multivew is only lowered in ac_nir_lower_ngg, so we have to fake it here. */
      if (nir->info.stage == MESA_SHADER_MESH && pipeline_key->has_multiview_view_index) {
         per_prim_mask |= VARYING_BIT_LAYER;
         info->uses_view_index = true;
      }

      /* Per vertex outputs. */
      outinfo->writes_pointsize = per_vtx_mask & VARYING_BIT_PSIZ;
      outinfo->writes_viewport_index = per_vtx_mask & VARYING_BIT_VIEWPORT;
      outinfo->writes_layer = per_vtx_mask & VARYING_BIT_LAYER;
      outinfo->writes_primitive_shading_rate = per_vtx_mask & VARYING_BIT_PRIMITIVE_SHADING_RATE;

      /* Per primitive outputs. */
      outinfo->writes_viewport_index_per_primitive = per_prim_mask & VARYING_BIT_VIEWPORT;
      outinfo->writes_layer_per_primitive = per_prim_mask & VARYING_BIT_LAYER;
      outinfo->writes_primitive_shading_rate_per_primitive = per_prim_mask & VARYING_BIT_PRIMITIVE_SHADING_RATE;

      /* Clip/cull distances. */
      outinfo->clip_dist_mask = (1 << nir->info.clip_distance_array_size) - 1;
      outinfo->cull_dist_mask = (1 << nir->info.cull_distance_array_size) - 1;
      outinfo->cull_dist_mask <<= nir->info.clip_distance_array_size;

      int pos_written = 0x1;

      if (outinfo->writes_pointsize || outinfo->writes_viewport_index || outinfo->writes_layer ||
          outinfo->writes_primitive_shading_rate)
         pos_written |= 1 << 1;

      unsigned num_clip_distances = util_bitcount(outinfo->clip_dist_mask);
      unsigned num_cull_distances = util_bitcount(outinfo->cull_dist_mask);

      if (num_clip_distances + num_cull_distances > 0)
         pos_written |= 1 << 2;
      if (num_clip_distances + num_cull_distances > 4)
         pos_written |= 1 << 3;

      outinfo->pos_exports = util_bitcount(pos_written);

      memset(outinfo->vs_output_param_offset, AC_EXP_PARAM_UNDEFINED,
             sizeof(outinfo->vs_output_param_offset));

      unsigned total_param_exports = 0;

      /* Per-vertex outputs */
      assign_outinfo_params(outinfo, per_vtx_mask, &total_param_exports);

      outinfo->param_exports = total_param_exports;

      /* Per-primitive outputs: the HW needs these to be last. */
      assign_outinfo_params(outinfo, per_prim_mask, &total_param_exports);

      outinfo->prim_param_exports = total_param_exports - outinfo->param_exports;
   }

   info->vs.needs_draw_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID);
   info->vs.needs_base_instance |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE);
   info->vs.needs_instance_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INSTANCE_ID);
   info->uses_view_index |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_VIEW_INDEX);
   info->uses_invocation_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_INVOCATION_ID);
   info->uses_prim_id |= BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_PRIMITIVE_ID);

   /* Used by compute and mesh shaders. */
   info->cs.uses_grid_size = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_NUM_WORKGROUPS);
   info->cs.uses_local_invocation_idx = BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_LOCAL_INVOCATION_INDEX) |
                                        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_SUBGROUP_ID) |
                                        BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_NUM_SUBGROUPS);

   if (nir->info.stage == MESA_SHADER_COMPUTE || nir->info.stage == MESA_SHADER_TASK) {
      for (int i = 0; i < 3; ++i)
         info->cs.block_size[i] = nir->info.workgroup_size[i];
   }

   switch (nir->info.stage) {
   case MESA_SHADER_COMPUTE:
      gather_shader_info_cs(device, nir, pipeline_key, info);
      break;
   case MESA_SHADER_TASK:
      gather_shader_info_task(nir, info);
      break;
   case MESA_SHADER_FRAGMENT:
      gather_shader_info_fs(nir, pipeline_key, info);
      break;
   case MESA_SHADER_GEOMETRY:
      gather_shader_info_gs(nir, info);
      break;
   case MESA_SHADER_TESS_EVAL:
      gather_shader_info_tes(nir, info);
      break;
   case MESA_SHADER_TESS_CTRL:
      gather_shader_info_tcs(device, nir, pipeline_key, info);
      break;
   case MESA_SHADER_VERTEX:
      gather_shader_info_vs(device, nir, pipeline_key, info);
      break;
   case MESA_SHADER_MESH:
      gather_shader_info_mesh(nir, info);
      break;
   default:
      break;
   }

   info->wave_size = radv_get_wave_size(device, nir->info.stage, info);
   info->ballot_bit_size = radv_get_ballot_bit_size(device, nir->info.stage, info);

   switch (nir->info.stage) {
   case MESA_SHADER_COMPUTE:
   case MESA_SHADER_TASK:
      info->workgroup_size =
         ac_compute_cs_workgroup_size(nir->info.workgroup_size, false, UINT32_MAX);
      break;
   case MESA_SHADER_MESH:
      /* Already computed in gather_shader_info_mesh(). */
      break;
   default:
      /* FS always operates without workgroups. Other stages are computed during linking but assume
       * no workgroups by default.
       */
      info->workgroup_size = info->wave_size;
      break;
   }
}

static void
gfx9_get_gs_info(const struct radv_device *device, struct radv_pipeline_stage *es_stage,
                 struct radv_pipeline_stage *gs_stage)
{
   const enum amd_gfx_level gfx_level = device->physical_device->rad_info.gfx_level;
   struct radv_shader_info *gs_info = &gs_stage->info;
   struct radv_shader_info *es_info = &es_stage->info;
   struct gfx9_gs_info *out = &gs_stage->info.gs_ring_info;

   const unsigned gs_num_invocations = MAX2(gs_info->gs.invocations, 1);
   const bool uses_adjacency = gs_info->gs.input_prim == SHADER_PRIM_LINES_ADJACENCY ||
                               gs_info->gs.input_prim == SHADER_PRIM_TRIANGLES_ADJACENCY;

   /* All these are in dwords: */
   /* We can't allow using the whole LDS, because GS waves compete with
    * other shader stages for LDS space. */
   const unsigned max_lds_size = 8 * 1024;
   const unsigned esgs_itemsize = es_info->esgs_itemsize / 4;
   unsigned esgs_lds_size;

   /* All these are per subgroup: */
   const unsigned max_out_prims = 32 * 1024;
   const unsigned max_es_verts = 255;
   const unsigned ideal_gs_prims = 64;
   unsigned max_gs_prims, gs_prims;
   unsigned min_es_verts, es_verts, worst_case_es_verts;

   if (uses_adjacency || gs_num_invocations > 1)
      max_gs_prims = 127 / gs_num_invocations;
   else
      max_gs_prims = 255;

   /* MAX_PRIMS_PER_SUBGROUP = gs_prims * max_vert_out * gs_invocations.
    * Make sure we don't go over the maximum value.
    */
   if (gs_info->gs.vertices_out > 0) {
      max_gs_prims =
         MIN2(max_gs_prims, max_out_prims / (gs_info->gs.vertices_out * gs_num_invocations));
   }
   assert(max_gs_prims > 0);

   /* If the primitive has adjacency, halve the number of vertices
    * that will be reused in multiple primitives.
    */
   min_es_verts = gs_info->gs.vertices_in / (uses_adjacency ? 2 : 1);

   gs_prims = MIN2(ideal_gs_prims, max_gs_prims);
   worst_case_es_verts = MIN2(min_es_verts * gs_prims, max_es_verts);

   /* Compute ESGS LDS size based on the worst case number of ES vertices
    * needed to create the target number of GS prims per subgroup.
    */
   esgs_lds_size = esgs_itemsize * worst_case_es_verts;

   /* If total LDS usage is too big, refactor partitions based on ratio
    * of ESGS item sizes.
    */
   if (esgs_lds_size > max_lds_size) {
      /* Our target GS Prims Per Subgroup was too large. Calculate
       * the maximum number of GS Prims Per Subgroup that will fit
       * into LDS, capped by the maximum that the hardware can support.
       */
      gs_prims = MIN2((max_lds_size / (esgs_itemsize * min_es_verts)), max_gs_prims);
      assert(gs_prims > 0);
      worst_case_es_verts = MIN2(min_es_verts * gs_prims, max_es_verts);

      esgs_lds_size = esgs_itemsize * worst_case_es_verts;
      assert(esgs_lds_size <= max_lds_size);
   }

   /* Now calculate remaining ESGS information. */
   if (esgs_lds_size)
      es_verts = MIN2(esgs_lds_size / esgs_itemsize, max_es_verts);
   else
      es_verts = max_es_verts;

   /* Vertices for adjacency primitives are not always reused, so restore
    * it for ES_VERTS_PER_SUBGRP.
    */
   min_es_verts = gs_info->gs.vertices_in;

   /* For normal primitives, the VGT only checks if they are past the ES
    * verts per subgroup after allocating a full GS primitive and if they
    * are, kick off a new subgroup.  But if those additional ES verts are
    * unique (e.g. not reused) we need to make sure there is enough LDS
    * space to account for those ES verts beyond ES_VERTS_PER_SUBGRP.
    */
   es_verts -= min_es_verts - 1;

   const uint32_t es_verts_per_subgroup = es_verts;
   const uint32_t gs_prims_per_subgroup = gs_prims;
   const uint32_t gs_inst_prims_in_subgroup = gs_prims * gs_num_invocations;
   const uint32_t max_prims_per_subgroup = gs_inst_prims_in_subgroup * gs_info->gs.vertices_out;
   out->lds_size = align(esgs_lds_size, 128) / 128;
   out->vgt_gs_onchip_cntl = S_028A44_ES_VERTS_PER_SUBGRP(es_verts_per_subgroup) |
                             S_028A44_GS_PRIMS_PER_SUBGRP(gs_prims_per_subgroup) |
                             S_028A44_GS_INST_PRIMS_IN_SUBGRP(gs_inst_prims_in_subgroup);
   out->vgt_gs_max_prims_per_subgroup = S_028A94_MAX_PRIMS_PER_SUBGROUP(max_prims_per_subgroup);
   out->vgt_esgs_ring_itemsize = esgs_itemsize;
   assert(max_prims_per_subgroup <= max_out_prims);

   unsigned workgroup_size = ac_compute_esgs_workgroup_size(gfx_level, es_info->wave_size,
      es_verts_per_subgroup, gs_inst_prims_in_subgroup);
   es_info->workgroup_size = workgroup_size;
   gs_info->workgroup_size = workgroup_size;
}

static void
clamp_gsprims_to_esverts(unsigned *max_gsprims, unsigned max_esverts, unsigned min_verts_per_prim,
                         bool use_adjacency)
{
   unsigned max_reuse = max_esverts - min_verts_per_prim;
   if (use_adjacency)
      max_reuse /= 2;
   *max_gsprims = MIN2(*max_gsprims, 1 + max_reuse);
}

static unsigned
radv_get_num_input_vertices(const struct radv_pipeline_stage *es_stage,
                            const struct radv_pipeline_stage *gs_stage)
{
   if (gs_stage) {
      return gs_stage->nir->info.gs.vertices_in;
   }

   if (es_stage->stage == MESA_SHADER_TESS_EVAL) {
      if (es_stage->nir->info.tess.point_mode)
         return 1;
      if (es_stage->nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         return 2;
      return 3;
   }

   return 3;
}

static unsigned
radv_get_pre_rast_input_topology(const struct radv_pipeline_stage *es_stage,
                                 const struct radv_pipeline_stage *gs_stage)
{
   if (gs_stage) {
      return gs_stage->nir->info.gs.input_primitive;
   }

   if (es_stage->stage == MESA_SHADER_TESS_EVAL) {
      if (es_stage->nir->info.tess.point_mode)
         return SHADER_PRIM_POINTS;
      if (es_stage->nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES)
         return SHADER_PRIM_LINES;
      return SHADER_PRIM_TRIANGLES;
   }

   return SHADER_PRIM_TRIANGLES;
}

static void
gfx10_get_ngg_info(const struct radv_device *device, struct radv_pipeline_stage *es_stage,
                   struct radv_pipeline_stage *gs_stage)
{
   const enum amd_gfx_level gfx_level = device->physical_device->rad_info.gfx_level;
   struct radv_shader_info *gs_info = gs_stage ? &gs_stage->info : NULL;
   struct radv_shader_info *es_info = &es_stage->info;
   const unsigned max_verts_per_prim = radv_get_num_input_vertices(es_stage, gs_stage);
   const unsigned min_verts_per_prim = gs_stage ? max_verts_per_prim : 1;
   struct gfx10_ngg_info *out = gs_stage ? &gs_info->ngg_info : &es_info->ngg_info;

   const unsigned gs_num_invocations = gs_stage ? MAX2(gs_info->gs.invocations, 1) : 1;

   const unsigned input_prim = radv_get_pre_rast_input_topology(es_stage, gs_stage);
   const bool uses_adjacency = input_prim == SHADER_PRIM_LINES_ADJACENCY ||
                               input_prim == SHADER_PRIM_TRIANGLES_ADJACENCY;

   /* All these are in dwords: */
   /* We can't allow using the whole LDS, because GS waves compete with
    * other shader stages for LDS space.
    *
    * TODO: We should really take the shader's internal LDS use into
    *       account. The linker will fail if the size is greater than
    *       8K dwords.
    */
   const unsigned max_lds_size = 8 * 1024 - 768;
   const unsigned target_lds_size = max_lds_size;
   unsigned esvert_lds_size = 0;
   unsigned gsprim_lds_size = 0;

   /* All these are per subgroup: */
   const unsigned min_esverts = gfx_level >= GFX10_3 ? 29 : 24;
   bool max_vert_out_per_gs_instance = false;
   unsigned max_esverts_base = 128;
   unsigned max_gsprims_base = 128; /* default prim group size clamp */

   /* Hardware has the following non-natural restrictions on the value
    * of GE_CNTL.VERT_GRP_SIZE based on based on the primitive type of
    * the draw:
    *  - at most 252 for any line input primitive type
    *  - at most 251 for any quad input primitive type
    *  - at most 251 for triangle strips with adjacency (this happens to
    *    be the natural limit for triangle *lists* with adjacency)
    */
   max_esverts_base = MIN2(max_esverts_base, 251 + max_verts_per_prim - 1);

   if (gs_stage) {
      unsigned max_out_verts_per_gsprim = gs_info->gs.vertices_out * gs_num_invocations;

      if (max_out_verts_per_gsprim <= 256) {
         if (max_out_verts_per_gsprim) {
            max_gsprims_base = MIN2(max_gsprims_base, 256 / max_out_verts_per_gsprim);
         }
      } else {
         /* Use special multi-cycling mode in which each GS
          * instance gets its own subgroup. Does not work with
          * tessellation. */
         max_vert_out_per_gs_instance = true;
         max_gsprims_base = 1;
         max_out_verts_per_gsprim = gs_info->gs.vertices_out;
      }

      esvert_lds_size = es_info->esgs_itemsize / 4;
      gsprim_lds_size = (gs_info->gs.gsvs_vertex_size / 4 + 1) * max_out_verts_per_gsprim;
   } else {
      /* VS and TES. */
      /* LDS size for passing data from GS to ES. */
      struct radv_streamout_info *so_info = &es_info->so;

      if (so_info->num_outputs)
         esvert_lds_size = 4 * so_info->num_outputs + 1;

      /* GS stores Primitive IDs (one DWORD) into LDS at the address
       * corresponding to the ES thread of the provoking vertex. All
       * ES threads load and export PrimitiveID for their thread.
       */
      if (es_stage->stage == MESA_SHADER_VERTEX && es_stage->info.outinfo.export_prim_id)
         esvert_lds_size = MAX2(esvert_lds_size, 1);
   }

   unsigned max_gsprims = max_gsprims_base;
   unsigned max_esverts = max_esverts_base;

   if (esvert_lds_size)
      max_esverts = MIN2(max_esverts, target_lds_size / esvert_lds_size);
   if (gsprim_lds_size)
      max_gsprims = MIN2(max_gsprims, target_lds_size / gsprim_lds_size);

   max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
   clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, uses_adjacency);
   assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);

   if (esvert_lds_size || gsprim_lds_size) {
      /* Now that we have a rough proportionality between esverts
       * and gsprims based on the primitive type, scale both of them
       * down simultaneously based on required LDS space.
       *
       * We could be smarter about this if we knew how much vertex
       * reuse to expect.
       */
      unsigned lds_total = max_esverts * esvert_lds_size + max_gsprims * gsprim_lds_size;
      if (lds_total > target_lds_size) {
         max_esverts = max_esverts * target_lds_size / lds_total;
         max_gsprims = max_gsprims * target_lds_size / lds_total;

         max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
         clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, uses_adjacency);
         assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);
      }
   }

   /* Round up towards full wave sizes for better ALU utilization. */
   if (!max_vert_out_per_gs_instance) {
      unsigned orig_max_esverts;
      unsigned orig_max_gsprims;
      unsigned wavesize;

      if (gs_stage) {
         wavesize = gs_info->wave_size;
      } else {
         wavesize = es_info->wave_size;
      }

      do {
         orig_max_esverts = max_esverts;
         orig_max_gsprims = max_gsprims;

         max_esverts = align(max_esverts, wavesize);
         max_esverts = MIN2(max_esverts, max_esverts_base);
         if (esvert_lds_size)
            max_esverts =
               MIN2(max_esverts, (max_lds_size - max_gsprims * gsprim_lds_size) / esvert_lds_size);
         max_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);

         /* Hardware restriction: minimum value of max_esverts */
         if (gfx_level == GFX10)
            max_esverts = MAX2(max_esverts, min_esverts - 1 + max_verts_per_prim);
         else
            max_esverts = MAX2(max_esverts, min_esverts);

         max_gsprims = align(max_gsprims, wavesize);
         max_gsprims = MIN2(max_gsprims, max_gsprims_base);
         if (gsprim_lds_size) {
            /* Don't count unusable vertices to the LDS
             * size. Those are vertices above the maximum
             * number of vertices that can occur in the
             * workgroup, which is e.g. max_gsprims * 3
             * for triangles.
             */
            unsigned usable_esverts = MIN2(max_esverts, max_gsprims * max_verts_per_prim);
            max_gsprims = MIN2(max_gsprims,
                               (max_lds_size - usable_esverts * esvert_lds_size) / gsprim_lds_size);
         }
         clamp_gsprims_to_esverts(&max_gsprims, max_esverts, min_verts_per_prim, uses_adjacency);
         assert(max_esverts >= max_verts_per_prim && max_gsprims >= 1);
      } while (orig_max_esverts != max_esverts || orig_max_gsprims != max_gsprims);

      /* Verify the restriction. */
      if (gfx_level == GFX10)
         assert(max_esverts >= min_esverts - 1 + max_verts_per_prim);
      else
         assert(max_esverts >= min_esverts);
   } else {
      /* Hardware restriction: minimum value of max_esverts */
      if (gfx_level == GFX10)
         max_esverts = MAX2(max_esverts, min_esverts - 1 + max_verts_per_prim);
      else
         max_esverts = MAX2(max_esverts, min_esverts);
   }

   unsigned max_out_vertices = max_vert_out_per_gs_instance ? gs_info->gs.vertices_out
                               : gs_stage
                                  ? max_gsprims * gs_num_invocations * gs_info->gs.vertices_out
                                  : max_esverts;
   assert(max_out_vertices <= 256);

   unsigned prim_amp_factor = 1;
   if (gs_stage) {
      /* Number of output primitives per GS input primitive after
       * GS instancing. */
      prim_amp_factor = gs_info->gs.vertices_out;
   }

   /* On Gfx10, the GE only checks against the maximum number of ES verts
    * after allocating a full GS primitive. So we need to ensure that
    * whenever this check passes, there is enough space for a full
    * primitive without vertex reuse.
    */
   if (gfx_level == GFX10)
      out->hw_max_esverts = max_esverts - max_verts_per_prim + 1;
   else
      out->hw_max_esverts = max_esverts;

   out->max_gsprims = max_gsprims;
   out->max_out_verts = max_out_vertices;
   out->prim_amp_factor = prim_amp_factor;
   out->max_vert_out_per_gs_instance = max_vert_out_per_gs_instance;
   out->ngg_emit_size = max_gsprims * gsprim_lds_size;
   out->enable_vertex_grouping = true;

   /* Don't count unusable vertices. */
   out->esgs_ring_size = MIN2(max_esverts, max_gsprims * max_verts_per_prim) * esvert_lds_size * 4;

   if (gs_stage) {
      out->vgt_esgs_ring_itemsize = es_info->esgs_itemsize / 4;
   } else {
      out->vgt_esgs_ring_itemsize = 1;
   }

   assert(out->hw_max_esverts >= min_esverts); /* HW limitation */

   unsigned workgroup_size =
      ac_compute_ngg_workgroup_size(
         max_esverts, max_gsprims * gs_num_invocations, max_out_vertices, prim_amp_factor);
   if (gs_stage) {
      gs_info->workgroup_size = workgroup_size;
   }
   es_info->workgroup_size = workgroup_size;
}

static void
radv_determine_ngg_settings(struct radv_device *device, struct radv_pipeline_stage *es_stage,
                            struct radv_pipeline_stage *fs_stage,
                            const struct radv_pipeline_key *pipeline_key)
{
   assert(es_stage->stage == MESA_SHADER_VERTEX || es_stage->stage == MESA_SHADER_TESS_EVAL);
   assert(fs_stage->stage == MESA_SHADER_FRAGMENT);

   uint64_t ps_inputs_read = fs_stage->nir->info.inputs_read;

   unsigned num_vertices_per_prim = si_conv_prim_to_gs_out(pipeline_key->vs.topology) + 1;
   if (es_stage->stage == MESA_SHADER_TESS_EVAL) {
      num_vertices_per_prim = es_stage->nir->info.tess.point_mode ? 1 :
         es_stage->nir->info.tess._primitive_mode == TESS_PRIMITIVE_ISOLINES ? 2 : 3;
   }

   /* TODO: Enable culling for LLVM. */
   es_stage->info.has_ngg_culling =
      radv_consider_culling(device->physical_device, es_stage->nir, ps_inputs_read,
                            num_vertices_per_prim, &es_stage->info) &&
      !radv_use_llvm_for_stage(device, es_stage->stage);

   nir_function_impl *impl = nir_shader_get_entrypoint(es_stage->nir);
   es_stage->info.has_ngg_early_prim_export = exec_list_is_singular(&impl->body);

   /* Invocations that process an input vertex */
   const struct gfx10_ngg_info *ngg_info = &es_stage->info.ngg_info;
   unsigned max_vtx_in = MIN2(256, ngg_info->enable_vertex_grouping ?
         ngg_info->hw_max_esverts : num_vertices_per_prim * ngg_info->max_gsprims);

   unsigned lds_bytes_if_culling_off = 0;
   /* We need LDS space when VS needs to export the primitive ID. */
   if (es_stage->stage == MESA_SHADER_VERTEX && es_stage->info.outinfo.export_prim_id)
      lds_bytes_if_culling_off = max_vtx_in * 4u;

   es_stage->info.num_lds_blocks_when_not_culling =
      DIV_ROUND_UP(lds_bytes_if_culling_off,
                   device->physical_device->rad_info.lds_encode_granularity);

   /* NGG passthrough mode should be disabled when culling and when the vertex shader
    * exports the primitive ID.
    */
   es_stage->info.is_ngg_passthrough = !es_stage->info.has_ngg_culling &&
      !(es_stage->stage == MESA_SHADER_VERTEX && es_stage->info.outinfo.export_prim_id);
}

static void
radv_link_shaders_info(struct radv_device *device,
                       struct radv_pipeline_stage *producer, struct radv_pipeline_stage *consumer,
                       const struct radv_pipeline_key *pipeline_key)
{
   /* Export primitive ID or clip/cull distances if necessary. */
   if (consumer->stage == MESA_SHADER_FRAGMENT) {
      struct radv_vs_output_info *outinfo = &producer->info.outinfo;
      const bool ps_prim_id_in = consumer->info.ps.prim_id_input;
      const bool ps_clip_dists_in = !!consumer->info.ps.num_input_clips_culls;

      if (ps_prim_id_in &&
          (producer->stage == MESA_SHADER_VERTEX || producer->stage == MESA_SHADER_TESS_EVAL)) {
         /* Mark the primitive ID as output when it's implicitly exported by VS or TES with NGG. */
         if (outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] == AC_EXP_PARAM_UNDEFINED)
            outinfo->vs_output_param_offset[VARYING_SLOT_PRIMITIVE_ID] = outinfo->param_exports++;

         outinfo->export_prim_id = true;
      }

      if (ps_clip_dists_in) {
         if (producer->nir->info.outputs_written & VARYING_BIT_CLIP_DIST0)
            outinfo->vs_output_param_offset[VARYING_SLOT_CLIP_DIST0] = outinfo->param_exports++;
         if (producer->nir->info.outputs_written & VARYING_BIT_CLIP_DIST1)
            outinfo->vs_output_param_offset[VARYING_SLOT_CLIP_DIST1] = outinfo->param_exports++;

         outinfo->export_clip_dists = true;
      }
   }

   if (producer->stage == MESA_SHADER_VERTEX || producer->stage == MESA_SHADER_TESS_EVAL) {
      if (consumer->stage == MESA_SHADER_GEOMETRY) {
         uint32_t num_outputs_written;

         if (producer->stage == MESA_SHADER_TESS_EVAL) {
            num_outputs_written = producer->info.tes.num_linked_outputs;
            producer->info.tes.as_es = true;
         } else {
            num_outputs_written = producer->info.vs.num_linked_outputs;
            producer->info.vs.as_es = true;
         }

         /* Compute the ESGS item size for VS or TES as ES. */
         producer->info.esgs_itemsize = num_outputs_written * 16;
      }

      /* Compute NGG info (GFX10+) or GS info. */
      if (producer->info.is_ngg) {
         struct radv_pipeline_stage *gs_stage =
            consumer->stage == MESA_SHADER_GEOMETRY ? consumer : NULL;

         gfx10_get_ngg_info(device, producer, gs_stage);

         /* Determine other NGG settings like culling for VS or TES without GS. */
         if (!gs_stage) {
            radv_determine_ngg_settings(device, producer, consumer, pipeline_key);
         }
      } else if (consumer->stage == MESA_SHADER_GEOMETRY) {
         gfx9_get_gs_info(device, producer, consumer);
      }
   }

   if (producer->stage == MESA_SHADER_VERTEX && consumer->stage == MESA_SHADER_TESS_CTRL) {
      struct radv_pipeline_stage *vs_stage = producer;
      struct radv_pipeline_stage *tcs_stage = consumer;

      vs_stage->info.vs.as_ls = true;

      if (pipeline_key->dynamic_patch_control_points) {
         /* Set the workgroup size to the maximum possible value to ensure that compilers don't
          * optimize barriers.
          */
         vs_stage->info.workgroup_size = 256;
         tcs_stage->info.workgroup_size = 256;
      } else {
         vs_stage->info.workgroup_size =
            ac_compute_lshs_workgroup_size(device->physical_device->rad_info.gfx_level,
                                           MESA_SHADER_VERTEX, tcs_stage->info.num_tess_patches,
                                           pipeline_key->tcs.tess_input_vertices,
                                           tcs_stage->info.tcs.tcs_vertices_out);

         tcs_stage->info.workgroup_size =
            ac_compute_lshs_workgroup_size(device->physical_device->rad_info.gfx_level,
                                           MESA_SHADER_TESS_CTRL, tcs_stage->info.num_tess_patches,
                                           pipeline_key->tcs.tess_input_vertices,
                                           tcs_stage->info.tcs.tcs_vertices_out);

         if (!radv_use_llvm_for_stage(device, MESA_SHADER_VERTEX)) {
            /* When the number of TCS input and output vertices are the same (typically 3):
             * - There is an equal amount of LS and HS invocations
             * - In case of merged LSHS shaders, the LS and HS halves of the shader always process
             *   the exact same vertex. We can use this knowledge to optimize them.
             *
             * We don't set tcs_in_out_eq if the float controls differ because that might involve
             * different float modes for the same block and our optimizer doesn't handle a
             * instruction dominating another with a different mode.
             */
            vs_stage->info.vs.tcs_in_out_eq =
               device->physical_device->rad_info.gfx_level >= GFX9 &&
               pipeline_key->tcs.tess_input_vertices == tcs_stage->info.tcs.tcs_vertices_out &&
               vs_stage->nir->info.float_controls_execution_mode ==
                  tcs_stage->nir->info.float_controls_execution_mode;

            if (vs_stage->info.vs.tcs_in_out_eq)
               vs_stage->info.vs.tcs_temp_only_input_mask =
                  tcs_stage->nir->info.inputs_read &
                  vs_stage->nir->info.outputs_written &
                  ~tcs_stage->nir->info.tess.tcs_cross_invocation_inputs_read &
                  ~tcs_stage->nir->info.inputs_read_indirectly &
                  ~vs_stage->nir->info.outputs_accessed_indirectly;
         }
      }
   }

   /* Copy shader info between TCS<->TES. */
   if (producer->stage == MESA_SHADER_TESS_CTRL) {
      struct radv_pipeline_stage *tcs_stage = producer;
      struct radv_pipeline_stage *tes_stage = consumer;

      tcs_stage->info.tcs.tes_reads_tess_factors =
         !!(tes_stage->nir->info.inputs_read & (VARYING_BIT_TESS_LEVEL_INNER | VARYING_BIT_TESS_LEVEL_OUTER));
      tcs_stage->info.tcs.tes_inputs_read = tes_stage->nir->info.inputs_read;
      tcs_stage->info.tcs.tes_patch_inputs_read = tes_stage->nir->info.patch_inputs_read;

      tes_stage->info.num_tess_patches = tcs_stage->info.num_tess_patches;
   }

   /* Task/mesh I/O uses the task ring buffers. */
   if (producer->stage == MESA_SHADER_TASK) {
      consumer->info.ms.has_task = true;
   }
}

static void
radv_nir_shader_info_merge(const struct radv_pipeline_stage *src, struct radv_pipeline_stage *dst)
{
   const struct radv_shader_info *src_info = &src->info;
   struct radv_shader_info *dst_info = &dst->info;

   assert((src->stage == MESA_SHADER_VERTEX && dst->stage == MESA_SHADER_TESS_CTRL) ||
          (src->stage == MESA_SHADER_VERTEX && dst->stage == MESA_SHADER_GEOMETRY) ||
          (src->stage == MESA_SHADER_TESS_EVAL && dst->stage == MESA_SHADER_GEOMETRY));

   dst_info->loads_push_constants |= src_info->loads_push_constants;
   dst_info->loads_dynamic_offsets |= src_info->loads_dynamic_offsets;
   dst_info->desc_set_used_mask |= src_info->desc_set_used_mask;
   dst_info->uses_view_index |= src_info->uses_view_index;
   dst_info->uses_invocation_id |= src_info->uses_invocation_id;
   dst_info->uses_prim_id |= src_info->uses_prim_id;
   dst_info->inline_push_constant_mask |= src_info->inline_push_constant_mask;

   /* Only inline all push constants if both allows it. */
   dst_info->can_inline_all_push_constants &= src_info->can_inline_all_push_constants;

   if (src->stage == MESA_SHADER_VERTEX) {
      dst_info->vs = src_info->vs;
   } else {
      dst_info->tes = src_info->tes;
   }

   if (dst->stage == MESA_SHADER_GEOMETRY) {
      dst_info->is_ngg = src_info->is_ngg;
      dst_info->gs.es_type = src->stage;
   }
}

static const gl_shader_stage graphics_shader_order[] = {
   MESA_SHADER_VERTEX,
   MESA_SHADER_TESS_CTRL,
   MESA_SHADER_TESS_EVAL,
   MESA_SHADER_GEOMETRY,

   MESA_SHADER_TASK,
   MESA_SHADER_MESH,
};

void
radv_nir_shader_info_link(struct radv_device *device, const struct radv_pipeline_key *pipeline_key,
                          struct radv_pipeline_stage *stages)
{
   /* Walk backwards to link */
   struct radv_pipeline_stage *next_stage = &stages[MESA_SHADER_FRAGMENT];
   for (int i = ARRAY_SIZE(graphics_shader_order) - 1; i >= 0; i--) {
      gl_shader_stage s = graphics_shader_order[i];
      if (!stages[s].nir)
         continue;

      radv_link_shaders_info(device, &stages[s], next_stage, pipeline_key);
      next_stage = &stages[s];
   }

   if (device->physical_device->rad_info.gfx_level >= GFX9) {
      /* Merge shader info for VS+TCS. */
      if (stages[MESA_SHADER_TESS_CTRL].nir) {
         radv_nir_shader_info_merge(&stages[MESA_SHADER_VERTEX], &stages[MESA_SHADER_TESS_CTRL]);
      }

      /* Merge shader info for VS+GS or TES+GS. */
      if (stages[MESA_SHADER_GEOMETRY].nir) {
         gl_shader_stage pre_stage =
            stages[MESA_SHADER_TESS_EVAL].nir ? MESA_SHADER_TESS_EVAL : MESA_SHADER_VERTEX;

         radv_nir_shader_info_merge(&stages[pre_stage], &stages[MESA_SHADER_GEOMETRY]);
      }
   }
}
