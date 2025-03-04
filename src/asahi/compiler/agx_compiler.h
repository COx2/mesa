/*
 * Copyright (C) 2021 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2020 Collabora Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __AGX_COMPILER_H
#define __AGX_COMPILER_H

#include "compiler/nir/nir.h"
#include "util/u_math.h"
#include "util/half_float.h"
#include "util/u_dynarray.h"
#include "util/u_worklist.h"
#include "agx_compile.h"
#include "agx_opcodes.h"
#include "agx_minifloat.h"

#ifdef __cplusplus
extern "C" {
#endif

enum agx_dbg {
   AGX_DBG_MSGS        = BITFIELD_BIT(0),
   AGX_DBG_SHADERS     = BITFIELD_BIT(1),
   AGX_DBG_SHADERDB    = BITFIELD_BIT(2),
   AGX_DBG_VERBOSE     = BITFIELD_BIT(3),
   AGX_DBG_INTERNAL    = BITFIELD_BIT(4),
   AGX_DBG_NOVALIDATE  = BITFIELD_BIT(5),
   AGX_DBG_NOOPT       = BITFIELD_BIT(6),
};

extern int agx_debug;

/* r0-r127 inclusive, as pairs of 16-bits, gives 256 registers */
#define AGX_NUM_REGS (256)

enum agx_index_type {
   AGX_INDEX_NULL = 0,
   AGX_INDEX_NORMAL = 1,
   AGX_INDEX_IMMEDIATE = 2,
   AGX_INDEX_UNIFORM = 3,
   AGX_INDEX_REGISTER = 4,
};

enum agx_size {
   AGX_SIZE_16 = 0,
   AGX_SIZE_32 = 1,
   AGX_SIZE_64 = 2
};

static inline unsigned
agx_size_align_16(enum agx_size size)
{
   switch (size) {
   case AGX_SIZE_16: return 1;
   case AGX_SIZE_32: return 2;
   case AGX_SIZE_64: return 4;
   }

   unreachable("Invalid size");
}

typedef struct {
   /* Sufficient for as many SSA values as we need. Immediates and uniforms fit in 16-bits */
   unsigned value : 22;

   /* Indicates that this source kills the referenced value (because it is the
    * last use in a block and the source is not live after the block). Set by
    * liveness analysis. */
   bool kill : 1;

   /* Cache hints */
   bool cache : 1;
   bool discard : 1;

   /* src - float modifiers */
   bool abs : 1;
   bool neg : 1;

   enum agx_size size : 2;
   enum agx_index_type type : 3;
} agx_index;

static inline agx_index
agx_get_index(unsigned value, enum agx_size size)
{
   return (agx_index) {
      .value = value,
      .size = size,
      .type = AGX_INDEX_NORMAL,
   };
}

static inline agx_index
agx_immediate(uint16_t imm)
{
   return (agx_index) {
      .value = imm,
      .size = AGX_SIZE_32,
      .type = AGX_INDEX_IMMEDIATE,
   };
}

static inline agx_index
agx_immediate_f(float f)
{
   assert(agx_minifloat_exact(f));
   return agx_immediate(agx_minifloat_encode(f));
}

/* in half-words, specify r0h as 1, r1 as 2... */
static inline agx_index
agx_register(uint8_t imm, enum agx_size size)
{
   return (agx_index) {
      .value = imm,
      .size = size,
      .type = AGX_INDEX_REGISTER,
   };
}

/* Also in half-words */
static inline agx_index
agx_uniform(uint8_t imm, enum agx_size size)
{
   return (agx_index) {
      .value = imm,
      .size = size,
      .type = AGX_INDEX_UNIFORM,
   };
}

static inline agx_index
agx_null()
{
   return (agx_index) { .type = AGX_INDEX_NULL };
}

static inline agx_index
agx_zero()
{
   return agx_immediate(0);
}

/* IEEE 754 additive identity -0.0, stored as an 8-bit AGX minifloat: mantissa
 * = exponent = 0, sign bit set */

static inline agx_index
agx_negzero()
{
   return agx_immediate(0x80);
}

static inline agx_index
agx_abs(agx_index idx)
{
   idx.abs = true;
   idx.neg = false;
   return idx;
}

static inline agx_index
agx_neg(agx_index idx)
{
   idx.neg ^= true;
   return idx;
}

/* Replaces an index, preserving any modifiers */

static inline agx_index
agx_replace_index(agx_index old, agx_index replacement)
{
   replacement.abs = old.abs;
   replacement.neg = old.neg;
   return replacement;
}

static inline bool
agx_is_null(agx_index idx)
{
   return idx.type == AGX_INDEX_NULL;
}

/* Compares equivalence as references */

static inline bool
agx_is_equiv(agx_index left, agx_index right)
{
   return (left.type == right.type) && (left.value == right.value);
}

#define AGX_MAX_DESTS 4
#define AGX_MAX_SRCS 5

enum agx_icond {
   AGX_ICOND_UEQ = 0,
   AGX_ICOND_ULT = 1,
   AGX_ICOND_UGT = 2,
   /* unknown */
   AGX_ICOND_SEQ = 4,
   AGX_ICOND_SLT = 5,
   AGX_ICOND_SGT = 6,
   /* unknown */
};

enum agx_fcond {
   AGX_FCOND_EQ = 0,
   AGX_FCOND_LT = 1,
   AGX_FCOND_GT = 2,
   AGX_FCOND_LTN = 3,
   /* unknown */
   AGX_FCOND_GE = 5,
   AGX_FCOND_LE = 6,
   AGX_FCOND_GTN = 7,
};

enum agx_round {
   AGX_ROUND_RTZ = 0,
   AGX_ROUND_RTE = 1,
};

enum agx_convert {
   AGX_CONVERT_U8_TO_F = 0,
   AGX_CONVERT_S8_TO_F = 1,
   AGX_CONVERT_F_TO_U16 = 4,
   AGX_CONVERT_F_TO_S16 = 5,
   AGX_CONVERT_U16_TO_F = 6,
   AGX_CONVERT_S16_TO_F = 7,
   AGX_CONVERT_F_TO_U32 = 8,
   AGX_CONVERT_F_TO_S32 = 9,
   AGX_CONVERT_U32_TO_F = 10,
   AGX_CONVERT_S32_TO_F = 11
};

enum agx_lod_mode {
   AGX_LOD_MODE_AUTO_LOD = 0,
   AGX_LOD_MODE_AUTO_LOD_BIAS = 5,
   AGX_LOD_MODE_LOD_MIN = 6,
   AGX_LOD_MODE_LOD_GRAD = 4,
   AGX_LOD_MODE_LOD_GRAD_MIN = 12
};

enum agx_dim {
   AGX_DIM_TEX_1D = 0,
   AGX_DIM_TEX_1D_ARRAY = 1,
   AGX_DIM_TEX_2D = 2,
   AGX_DIM_TEX_2D_ARRAY = 3,
   AGX_DIM_TEX_2D_MS = 4,
   AGX_DIM_TEX_3D = 5,
   AGX_DIM_TEX_CUBE = 6,
   AGX_DIM_TEX_CUBE_ARRAY = 7
};

/* Forward declare for branch target */
struct agx_block;

typedef struct {
   /* Must be first */
   struct list_head link;

   /* The sources list.
    *
    * As a special case to workaround ordering issues when translating phis, if
    * nr_srcs == 0 and the opcode is PHI, holds a pointer to the NIR phi node.
    */
   union {
      agx_index *src;
      nir_phi_instr *phi;
   };

   enum agx_opcode op;

   /* Data flow */
   agx_index dest[AGX_MAX_DESTS];

   unsigned nr_srcs;

   union {
      uint32_t imm;
      uint32_t writeout;
      uint32_t truth_table;
      uint32_t component;
      uint32_t channels;
      uint32_t bfi_mask;
      enum agx_sr sr;
      enum agx_icond icond;
      enum agx_fcond fcond;
      enum agx_format format;
      enum agx_round round;
      enum agx_lod_mode lod_mode;
      struct agx_block *target;
   };

   /* For load varying */
   bool perspective : 1;

   /* Invert icond/fcond */
   bool invert_cond : 1;

   /* TODO: Handle tex ops more efficient */
   enum agx_dim dim : 3;
   bool offset : 1;
   bool shadow : 1;

   /* Final st_vary op */
   bool last : 1;

   /* Shift for a bitwise or memory op (conflicts with format for memory ops) */
   unsigned shift : 4;

   /* Scoreboard index, 0 or 1. Leave as 0 for instructions that do not require
    * scoreboarding (everything but memory load/store and texturing). */
   unsigned scoreboard : 1;

   /* Number of nested control flow layers to jump by */
   unsigned nest : 2;

   /* Output modifiers */
   bool saturate : 1;
   unsigned mask : 4;
} agx_instr;

struct agx_block;

typedef struct agx_block {
   /* Link to next block. Must be first */
   struct list_head link;

   /* List of instructions emitted for the current block */
   struct list_head instructions;

   /* Index of the block in source order */
   unsigned index;

   /* Control flow graph */
   struct agx_block *successors[2];
   struct util_dynarray predecessors;
   bool unconditional_jumps;

   /* Liveness analysis results */
   BITSET_WORD *live_in;
   BITSET_WORD *live_out;

   /* Register allocation */
   BITSET_DECLARE(regs_out, AGX_NUM_REGS);

   /* Offset of the block in the emitted binary */
   off_t offset;

   /** Available for passes to use for metadata */
   uint8_t pass_flags;
} agx_block;

typedef struct {
   nir_shader *nir;
   gl_shader_stage stage;
   struct list_head blocks; /* list of agx_block */
   struct agx_shader_info *out;
   struct agx_shader_key *key;

   /* Place to start pushing new values */
   unsigned push_base;

   /* Maximum block index */
   unsigned num_blocks;

   /* For creating temporaries */
   unsigned alloc;

   /* I don't really understand how writeout ops work yet */
   bool did_writeout;

   /* Has r0l been zeroed yet due to control flow? */
   bool any_cf;

   /* Number of nested control flow structures within the innermost loop. Since
    * NIR is just loop and if-else, this is the number of nested if-else
    * statements in the loop */
   unsigned loop_nesting;

   /* During instruction selection, for inserting control flow */
   agx_block *current_block;
   agx_block *continue_block;
   agx_block *break_block;
   agx_block *after_block;
   agx_block **indexed_nir_blocks;

   /* During instruction selection, map from vector agx_index to its scalar
    * components, populated by a split. */
   struct hash_table_u64 *allocated_vec;

   /* Stats for shader-db */
   unsigned loop_count;
   unsigned spills;
   unsigned fills;
} agx_context;

static inline void
agx_remove_instruction(agx_instr *ins)
{
   list_del(&ins->link);
}

static inline agx_index
agx_temp(agx_context *ctx, enum agx_size size)
{
   return agx_get_index(ctx->alloc++, size);
}

static enum agx_size
agx_size_for_bits(unsigned bits)
{
   switch (bits) {
   case 1:
   case 16: return AGX_SIZE_16;
   case 32: return AGX_SIZE_32;
   case 64: return AGX_SIZE_64;
   default: unreachable("Invalid bitsize");
   }
}

static inline agx_index
agx_src_index(nir_src *src)
{
   assert(src->is_ssa);

   return agx_get_index(src->ssa->index,
         agx_size_for_bits(nir_src_bit_size(*src)));
}

static inline agx_index
agx_dest_index(nir_dest *dst)
{
   assert(dst->is_ssa);

   return agx_get_index(dst->ssa.index,
         agx_size_for_bits(nir_dest_bit_size(*dst)));
}

static inline agx_index
agx_vec_for_dest(agx_context *ctx, nir_dest *dest)
{
   return agx_temp(ctx, agx_size_for_bits(nir_dest_bit_size(*dest)));
}

static inline agx_index
agx_vec_for_intr(agx_context *ctx, nir_intrinsic_instr *instr)
{
   return agx_vec_for_dest(ctx, &instr->dest);
}

/* Iterators for AGX IR */

#define agx_foreach_block(ctx, v) \
   list_for_each_entry(agx_block, v, &ctx->blocks, link)

#define agx_foreach_block_rev(ctx, v) \
   list_for_each_entry_rev(agx_block, v, &ctx->blocks, link)

#define agx_foreach_block_from(ctx, from, v) \
   list_for_each_entry_from(agx_block, v, from, &ctx->blocks, link)

#define agx_foreach_block_from_rev(ctx, from, v) \
   list_for_each_entry_from_rev(agx_block, v, from, &ctx->blocks, link)

#define agx_foreach_instr_in_block(block, v) \
   list_for_each_entry(agx_instr, v, &(block)->instructions, link)

#define agx_foreach_instr_in_block_rev(block, v) \
   list_for_each_entry_rev(agx_instr, v, &(block)->instructions, link)

#define agx_foreach_instr_in_block_safe(block, v) \
   list_for_each_entry_safe(agx_instr, v, &(block)->instructions, link)

#define agx_foreach_instr_in_block_safe_rev(block, v) \
   list_for_each_entry_safe_rev(agx_instr, v, &(block)->instructions, link)

#define agx_foreach_instr_in_block_from(block, v, from) \
   list_for_each_entry_from(agx_instr, v, from, &(block)->instructions, link)

#define agx_foreach_instr_in_block_from_rev(block, v, from) \
   list_for_each_entry_from_rev(agx_instr, v, from, &(block)->instructions, link)

#define agx_foreach_instr_global(ctx, v) \
   agx_foreach_block(ctx, v_block) \
      agx_foreach_instr_in_block(v_block, v)

#define agx_foreach_instr_global_rev(ctx, v) \
   agx_foreach_block_rev(ctx, v_block) \
      agx_foreach_instr_in_block_rev(v_block, v)

#define agx_foreach_instr_global_safe(ctx, v) \
   agx_foreach_block(ctx, v_block) \
      agx_foreach_instr_in_block_safe(v_block, v)

#define agx_foreach_instr_global_safe_rev(ctx, v) \
   agx_foreach_block_rev(ctx, v_block) \
      agx_foreach_instr_in_block_safe_rev(v_block, v)

/* Based on set_foreach, expanded with automatic type casts */

#define agx_foreach_successor(blk, v) \
   agx_block *v; \
   agx_block **_v; \
   for (_v = (agx_block **) &blk->successors[0], \
         v = *_v; \
         v != NULL && _v < (agx_block **) &blk->successors[2]; \
         _v++, v = *_v) \

#define agx_foreach_predecessor(blk, v) \
   util_dynarray_foreach(&blk->predecessors, agx_block *, v)

#define agx_foreach_src(ins, v) \
   for (unsigned v = 0; v < ins->nr_srcs; ++v)

#define agx_foreach_dest(ins, v) \
   for (unsigned v = 0; v < ARRAY_SIZE(ins->dest); ++v)

/*
 * Find the index of a predecessor, used as the implicit order of phi sources.
 */
static inline unsigned
agx_predecessor_index(agx_block *succ, agx_block *pred)
{
   unsigned index = 0;

   agx_foreach_predecessor(succ, x) {
      if (*x == pred) return index;

      index++;
   }

   unreachable("Invalid predecessor");
}

static inline agx_instr *
agx_prev_op(agx_instr *ins)
{
   return list_last_entry(&(ins->link), agx_instr, link);
}

static inline agx_instr *
agx_next_op(agx_instr *ins)
{
   return list_first_entry(&(ins->link), agx_instr, link);
}

static inline agx_block *
agx_next_block(agx_block *block)
{
   return list_first_entry(&(block->link), agx_block, link);
}

static inline agx_block *
agx_exit_block(agx_context *ctx)
{
   agx_block *last = list_last_entry(&ctx->blocks, agx_block, link);
   assert(!last->successors[0] && !last->successors[1]);
   return last;
}

#define agx_worklist_init(ctx, w) u_worklist_init(w, ctx->num_blocks, ctx)
#define agx_worklist_push_head(w, block) u_worklist_push_head(w, block, index)
#define agx_worklist_push_tail(w, block) u_worklist_push_tail(w, block, index)
#define agx_worklist_peek_head(w) u_worklist_peek_head(w, agx_block, index)
#define agx_worklist_pop_head(w)  u_worklist_pop_head( w, agx_block, index)
#define agx_worklist_peek_tail(w) u_worklist_peek_tail(w, agx_block, index)
#define agx_worklist_pop_tail(w)  u_worklist_pop_tail( w, agx_block, index)

/* Like in NIR, for use with the builder */

enum agx_cursor_option {
   agx_cursor_after_block,
   agx_cursor_before_instr,
   agx_cursor_after_instr
};

typedef struct {
   enum agx_cursor_option option;

   union {
      agx_block *block;
      agx_instr *instr;
   };
} agx_cursor;

static inline agx_cursor
agx_after_block(agx_block *block)
{
   return (agx_cursor) {
      .option = agx_cursor_after_block,
      .block = block
   };
}

static inline agx_cursor
agx_before_instr(agx_instr *instr)
{
   return (agx_cursor) {
      .option = agx_cursor_before_instr,
      .instr = instr
   };
}

static inline agx_cursor
agx_after_instr(agx_instr *instr)
{
   return (agx_cursor) {
      .option = agx_cursor_after_instr,
      .instr = instr
   };
}

/*
 * Get a cursor inserting at the logical end of the block. In particular, this
 * is before branches or control flow instructions, which occur after the
 * logical end but before the physical end.
 */
static inline agx_cursor
agx_after_block_logical(agx_block *block)
{
   /* Search for a p_logical_end */
   agx_foreach_instr_in_block_rev(block, I) {
      if (I->op == AGX_OPCODE_P_LOGICAL_END)
         return agx_before_instr(I);
   }

   /* If there's no p_logical_end, use the physical end */
   return agx_after_block(block);
}

/* IR builder in terms of cursor infrastructure */

typedef struct {
   agx_context *shader;
   agx_cursor cursor;
} agx_builder;

static inline agx_builder
agx_init_builder(agx_context *ctx, agx_cursor cursor)
{
   return (agx_builder) {
      .shader = ctx,
      .cursor = cursor
   };
}

/* Insert an instruction at the cursor and move the cursor */

static inline void
agx_builder_insert(agx_cursor *cursor, agx_instr *I)
{
   switch (cursor->option) {
   case agx_cursor_after_instr:
      list_add(&I->link, &cursor->instr->link);
      cursor->instr = I;
      return;

   case agx_cursor_after_block:
      list_addtail(&I->link, &cursor->block->instructions);
      cursor->option = agx_cursor_after_instr;
      cursor->instr = I;
      return;

   case agx_cursor_before_instr:
      list_addtail(&I->link, &cursor->instr->link);
      cursor->option = agx_cursor_after_instr;
      cursor->instr = I;
      return;
   }

   unreachable("Invalid cursor option");
}

/* Uniform file management */

agx_index
agx_indexed_sysval(agx_context *ctx, enum agx_push_type type, enum agx_size size,
      unsigned index, unsigned length);

agx_index
agx_vbo_base(agx_context *ctx, unsigned vbo);

/* Routines defined for AIR */

void agx_print_instr(agx_instr *I, FILE *fp);
void agx_print_block(agx_block *block, FILE *fp);
void agx_print_shader(agx_context *ctx, FILE *fp);
void agx_optimizer(agx_context *ctx);
void agx_lower_pseudo(agx_context *ctx);
void agx_dce(agx_context *ctx);
void agx_ra(agx_context *ctx);
void agx_pack_binary(agx_context *ctx, struct util_dynarray *emission);

#ifndef NDEBUG
void agx_validate(agx_context *ctx, const char *after_str);
#else
static inline void agx_validate(UNUSED agx_context *ctx, UNUSED const char *after_str) { return; }
#endif

unsigned agx_write_registers(agx_instr *I, unsigned d);

struct agx_copy {
   /* Base register destination of the copy */
   unsigned dest;

   /* Base register source of the copy */
   unsigned src;

   /* Size of the copy */
   enum agx_size size;

   /* Whether the copy has been handled. Callers must leave to false. */
   bool done;
};

void
agx_emit_parallel_copies(agx_builder *b, struct agx_copy *copies, unsigned n);

void agx_compute_liveness(agx_context *ctx);
void agx_liveness_ins_update(BITSET_WORD *live, agx_instr *I);

bool agx_lower_resinfo(nir_shader *s);

#ifdef __cplusplus
} /* extern C */
#endif

#endif
