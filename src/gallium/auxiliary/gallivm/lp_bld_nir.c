/**************************************************************************
 *
 * Copyright 2019 Red Hat.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **************************************************************************/

#include "lp_bld_nir.h"
#include "lp_bld_arit.h"
#include "lp_bld_bitarit.h"
#include "lp_bld_const.h"
#include "lp_bld_gather.h"
#include "lp_bld_logic.h"
#include "lp_bld_quad.h"
#include "lp_bld_flow.h"
#include "lp_bld_struct.h"
#include "lp_bld_debug.h"
#include "lp_bld_printf.h"
#include "nir_deref.h"

static void visit_cf_list(struct lp_build_nir_context *bld_base,
                          struct exec_list *list);

static LLVMValueRef cast_type(struct lp_build_nir_context *bld_base, LLVMValueRef val,
                              nir_alu_type alu_type, unsigned bit_size)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   switch (alu_type) {
   case nir_type_float:
      switch (bit_size) {
      case 32:
         return LLVMBuildBitCast(builder, val, bld_base->base.vec_type, "");
      case 64:
         return LLVMBuildBitCast(builder, val, bld_base->dbl_bld.vec_type, "");
      default:
         assert(0);
         break;
      }
      break;
   case nir_type_int:
      switch (bit_size) {
      case 8:
         return LLVMBuildBitCast(builder, val, bld_base->int8_bld.vec_type, "");
      case 16:
         return LLVMBuildBitCast(builder, val, bld_base->int16_bld.vec_type, "");
      case 32:
         return LLVMBuildBitCast(builder, val, bld_base->int_bld.vec_type, "");
      case 64:
         return LLVMBuildBitCast(builder, val, bld_base->int64_bld.vec_type, "");
      default:
         assert(0);
         break;
      }
      break;
   case nir_type_uint:
      switch (bit_size) {
      case 8:
         return LLVMBuildBitCast(builder, val, bld_base->uint8_bld.vec_type, "");
      case 16:
         return LLVMBuildBitCast(builder, val, bld_base->uint16_bld.vec_type, "");
      case 32:
         return LLVMBuildBitCast(builder, val, bld_base->uint_bld.vec_type, "");
      case 64:
         return LLVMBuildBitCast(builder, val, bld_base->uint64_bld.vec_type, "");
      default:
         assert(0);
         break;
      }
      break;
   case nir_type_uint32:
      return LLVMBuildBitCast(builder, val, bld_base->uint_bld.vec_type, "");
   default:
      return val;
   }
   return NULL;
}


static struct lp_build_context *get_flt_bld(struct lp_build_nir_context *bld_base,
                                            unsigned op_bit_size)
{
   if (op_bit_size == 64)
      return &bld_base->dbl_bld;
   else
      return &bld_base->base;
}

static unsigned glsl_sampler_to_pipe(int sampler_dim, bool is_array)
{
   unsigned pipe_target = PIPE_BUFFER;
   switch (sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:
      pipe_target = is_array ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
      break;
   case GLSL_SAMPLER_DIM_2D:
      pipe_target = is_array ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
      break;
   case GLSL_SAMPLER_DIM_3D:
      pipe_target = PIPE_TEXTURE_3D;
      break;
   case GLSL_SAMPLER_DIM_CUBE:
      pipe_target = is_array ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
      break;
   case GLSL_SAMPLER_DIM_RECT:
      pipe_target = PIPE_TEXTURE_RECT;
      break;
   case GLSL_SAMPLER_DIM_BUF:
      pipe_target = PIPE_BUFFER;
      break;
   default:
      break;
   }
   return pipe_target;
}

static LLVMValueRef get_ssa_src(struct lp_build_nir_context *bld_base, nir_ssa_def *ssa)
{
   return bld_base->ssa_defs[ssa->index];
}

static LLVMValueRef get_src(struct lp_build_nir_context *bld_base, nir_src src);

static LLVMValueRef get_reg_src(struct lp_build_nir_context *bld_base, nir_reg_src src)
{
   struct hash_entry *entry = _mesa_hash_table_search(bld_base->regs, src.reg);
   LLVMValueRef reg_storage = (LLVMValueRef)entry->data;
   struct lp_build_context *reg_bld = get_int_bld(bld_base, true, src.reg->bit_size);
   LLVMValueRef indir_src = NULL;
   if (src.indirect)
      indir_src = get_src(bld_base, *src.indirect);
   return bld_base->load_reg(bld_base, reg_bld, &src, indir_src, reg_storage);
}

static LLVMValueRef get_src(struct lp_build_nir_context *bld_base, nir_src src)
{
   if (src.is_ssa)
      return get_ssa_src(bld_base, src.ssa);
   else
      return get_reg_src(bld_base, src.reg);
}

static void assign_ssa(struct lp_build_nir_context *bld_base, int idx, LLVMValueRef ptr)
{
   bld_base->ssa_defs[idx] = ptr;
}

static void assign_ssa_dest(struct lp_build_nir_context *bld_base, const nir_ssa_def *ssa,
                            LLVMValueRef vals[NIR_MAX_VEC_COMPONENTS])
{
   assign_ssa(bld_base, ssa->index, ssa->num_components == 1 ? vals[0] : lp_nir_array_build_gather_values(bld_base->base.gallivm->builder, vals, ssa->num_components));
}

static void assign_reg(struct lp_build_nir_context *bld_base, const nir_reg_dest *reg,
                       unsigned write_mask,
                       LLVMValueRef vals[NIR_MAX_VEC_COMPONENTS])
{
   struct hash_entry *entry = _mesa_hash_table_search(bld_base->regs, reg->reg);
   LLVMValueRef reg_storage = (LLVMValueRef)entry->data;
   struct lp_build_context *reg_bld = get_int_bld(bld_base, true, reg->reg->bit_size);
   LLVMValueRef indir_src = NULL;
   if (reg->indirect)
      indir_src = get_src(bld_base, *reg->indirect);
   bld_base->store_reg(bld_base, reg_bld, reg, write_mask ? write_mask : 0xf, indir_src, reg_storage, vals);
}

static void assign_dest(struct lp_build_nir_context *bld_base, const nir_dest *dest, LLVMValueRef vals[NIR_MAX_VEC_COMPONENTS])
{
   if (dest->is_ssa)
      assign_ssa_dest(bld_base, &dest->ssa, vals);
   else
      assign_reg(bld_base, &dest->reg, 0, vals);
}

static void assign_alu_dest(struct lp_build_nir_context *bld_base, const nir_alu_dest *dest, LLVMValueRef vals[NIR_MAX_VEC_COMPONENTS])
{
   if (dest->dest.is_ssa)
      assign_ssa_dest(bld_base, &dest->dest.ssa, vals);
   else
      assign_reg(bld_base, &dest->dest.reg, dest->write_mask, vals);
}

static LLVMValueRef int_to_bool32(struct lp_build_nir_context *bld_base,
                                uint32_t src_bit_size,
                                bool is_unsigned,
                                LLVMValueRef val)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   struct lp_build_context *int_bld = get_int_bld(bld_base, is_unsigned, src_bit_size);
   LLVMValueRef result = lp_build_compare(bld_base->base.gallivm, int_bld->type, PIPE_FUNC_NOTEQUAL, val, int_bld->zero);
   if (src_bit_size == 64)
      result = LLVMBuildTrunc(builder, result, bld_base->int_bld.vec_type, "");
   return result;
}

static LLVMValueRef flt_to_bool32(struct lp_build_nir_context *bld_base,
                                  uint32_t src_bit_size,
                                  LLVMValueRef val)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   struct lp_build_context *flt_bld = get_flt_bld(bld_base, src_bit_size);
   LLVMValueRef result = lp_build_cmp(flt_bld, PIPE_FUNC_NOTEQUAL, val, flt_bld->zero);
   if (src_bit_size == 64)
      result = LLVMBuildTrunc(builder, result, bld_base->int_bld.vec_type, "");
   return result;
}

static LLVMValueRef fcmp32(struct lp_build_nir_context *bld_base,
                           enum pipe_compare_func compare,
                           uint32_t src_bit_size,
                           LLVMValueRef src[NIR_MAX_VEC_COMPONENTS])
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   struct lp_build_context *flt_bld = get_flt_bld(bld_base, src_bit_size);
   LLVMValueRef result;

   if (compare != PIPE_FUNC_NOTEQUAL)
      result = lp_build_cmp_ordered(flt_bld, compare, src[0], src[1]);
   else
      result = lp_build_cmp(flt_bld, compare, src[0], src[1]);
   if (src_bit_size == 64)
      result = LLVMBuildTrunc(builder, result, bld_base->int_bld.vec_type, "");
   return result;
}

static LLVMValueRef icmp32(struct lp_build_nir_context *bld_base,
                           enum pipe_compare_func compare,
                           bool is_unsigned,
                           uint32_t src_bit_size,
                           LLVMValueRef src[NIR_MAX_VEC_COMPONENTS])
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   struct lp_build_context *i_bld = get_int_bld(bld_base, is_unsigned, src_bit_size);
   LLVMValueRef result = lp_build_cmp(i_bld, compare, src[0], src[1]);
   if (src_bit_size < 32)
      result = LLVMBuildSExt(builder, result, bld_base->int_bld.vec_type, "");
   else if (src_bit_size == 64)
      result = LLVMBuildTrunc(builder, result, bld_base->int_bld.vec_type, "");
   return result;
}

static LLVMValueRef get_alu_src(struct lp_build_nir_context *bld_base,
                                nir_alu_src src,
                                unsigned num_components)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMValueRef value = get_src(bld_base, src.src);
   bool need_swizzle = false;

   assert(value);
   unsigned src_components = nir_src_num_components(src.src);
   for (unsigned i = 0; i < num_components; ++i) {
      assert(src.swizzle[i] < src_components);
      if (src.swizzle[i] != i)
         need_swizzle = true;
   }

   if (need_swizzle || num_components != src_components) {
      if (src_components > 1 && num_components == 1) {
         value = LLVMBuildExtractValue(gallivm->builder, value,
                                       src.swizzle[0], "");
      } else if (src_components == 1 && num_components > 1) {
         LLVMValueRef values[] = {value, value, value, value, value, value, value, value, value, value, value, value, value, value, value, value};
         value = lp_nir_array_build_gather_values(builder, values, num_components);
      } else {
         LLVMValueRef arr = LLVMGetUndef(LLVMArrayType(LLVMTypeOf(LLVMBuildExtractValue(builder, value, 0, "")), num_components));
         for (unsigned i = 0; i < num_components; i++)
            arr = LLVMBuildInsertValue(builder, arr, LLVMBuildExtractValue(builder, value, src.swizzle[i], ""), i, "");
         value = arr;
      }
   }
   assert(!src.negate);
   assert(!src.abs);
   return value;
}

static LLVMValueRef emit_b2f(struct lp_build_nir_context *bld_base,
                             LLVMValueRef src0,
                             unsigned bitsize)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   LLVMValueRef result = LLVMBuildAnd(builder, cast_type(bld_base, src0, nir_type_int, 32),
                                      LLVMBuildBitCast(builder, lp_build_const_vec(bld_base->base.gallivm, bld_base->base.type,
                                                                                   1.0), bld_base->int_bld.vec_type, ""),
                                      "");
   result = LLVMBuildBitCast(builder, result, bld_base->base.vec_type, "");
   switch (bitsize) {
   case 32:
      break;
   case 64:
      result = LLVMBuildFPExt(builder, result, bld_base->dbl_bld.vec_type, "");
      break;
   default:
      unreachable("unsupported bit size.");
   }
   return result;
}

static LLVMValueRef emit_b2i(struct lp_build_nir_context *bld_base,
                             LLVMValueRef src0,
                             unsigned bitsize)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   LLVMValueRef result = LLVMBuildAnd(builder, cast_type(bld_base, src0, nir_type_int, 32),
                                      lp_build_const_int_vec(bld_base->base.gallivm, bld_base->base.type, 1), "");
   switch (bitsize) {
   case 32:
      return result;
   case 64:
      return LLVMBuildZExt(builder, result, bld_base->int64_bld.vec_type, "");
   default:
      unreachable("unsupported bit size.");
   }
}

static LLVMValueRef emit_b32csel(struct lp_build_nir_context *bld_base,
                               unsigned src_bit_size[NIR_MAX_VEC_COMPONENTS],
                               LLVMValueRef src[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef sel = cast_type(bld_base, src[0], nir_type_int, 32);
   LLVMValueRef v = lp_build_compare(bld_base->base.gallivm, bld_base->int_bld.type, PIPE_FUNC_NOTEQUAL, sel, bld_base->int_bld.zero);
   struct lp_build_context *bld = get_int_bld(bld_base, false, src_bit_size[1]);
   return lp_build_select(bld, v, src[1], src[2]);
}

static LLVMValueRef split_64bit(struct lp_build_nir_context *bld_base,
                                LLVMValueRef src,
                                bool hi)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMValueRef shuffles[LP_MAX_VECTOR_WIDTH/32];
   LLVMValueRef shuffles2[LP_MAX_VECTOR_WIDTH/32];
   int len = bld_base->base.type.length * 2;
   for (unsigned i = 0; i < bld_base->base.type.length; i++) {
      shuffles[i] = lp_build_const_int32(gallivm, i * 2);
      shuffles2[i] = lp_build_const_int32(gallivm, (i * 2) + 1);
   }

   src = LLVMBuildBitCast(gallivm->builder, src, LLVMVectorType(LLVMInt32TypeInContext(gallivm->context), len), "");
   return LLVMBuildShuffleVector(gallivm->builder, src,
                                 LLVMGetUndef(LLVMTypeOf(src)),
                                 LLVMConstVector(hi ? shuffles2 : shuffles,
                                                 bld_base->base.type.length),
                                 "");
}

static LLVMValueRef
merge_64bit(struct lp_build_nir_context *bld_base,
            LLVMValueRef input,
            LLVMValueRef input2)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   int i;
   LLVMValueRef shuffles[2 * (LP_MAX_VECTOR_WIDTH/32)];
   int len = bld_base->base.type.length * 2;
   assert(len <= (2 * (LP_MAX_VECTOR_WIDTH/32)));

   for (i = 0; i < bld_base->base.type.length * 2; i+=2) {
      shuffles[i] = lp_build_const_int32(gallivm, i / 2);
      shuffles[i + 1] = lp_build_const_int32(gallivm, i / 2 + bld_base->base.type.length);
   }
   return LLVMBuildShuffleVector(builder, input, input2, LLVMConstVector(shuffles, len), "");
}

static LLVMValueRef
do_int_divide(struct lp_build_nir_context *bld_base,
              bool is_unsigned, unsigned src_bit_size,
              LLVMValueRef src, LLVMValueRef src2)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   struct lp_build_context *int_bld = get_int_bld(bld_base, is_unsigned, src_bit_size);
   struct lp_build_context *mask_bld = get_int_bld(bld_base, true, src_bit_size);
   LLVMValueRef div_mask = lp_build_cmp(mask_bld, PIPE_FUNC_EQUAL, src2,
                                        mask_bld->zero);

   if (!is_unsigned) {
      /* INT_MIN (0x80000000) / -1 (0xffffffff) causes sigfpe, seen with blender. */
      div_mask = LLVMBuildAnd(builder, div_mask, lp_build_const_int_vec(gallivm, int_bld->type, 0x7fffffff), "");
   }
   LLVMValueRef divisor = LLVMBuildOr(builder,
                                      div_mask,
                                      src2, "");
   LLVMValueRef result = lp_build_div(int_bld, src, divisor);

   if (!is_unsigned) {
      LLVMValueRef not_div_mask = LLVMBuildNot(builder, div_mask, "");
      return LLVMBuildAnd(builder, not_div_mask, result, "");
   } else
      /* udiv by zero is guaranteed to return 0xffffffff at least with d3d10
       * may as well do same for idiv */
      return LLVMBuildOr(builder, div_mask, result, "");
}

static LLVMValueRef
do_int_mod(struct lp_build_nir_context *bld_base,
           bool is_unsigned, unsigned src_bit_size,
           LLVMValueRef src, LLVMValueRef src2)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   struct lp_build_context *int_bld = get_int_bld(bld_base, is_unsigned, src_bit_size);
   LLVMValueRef div_mask = lp_build_cmp(int_bld, PIPE_FUNC_EQUAL, src2,
                                        int_bld->zero);
   LLVMValueRef divisor = LLVMBuildOr(builder,
                                      div_mask,
                                      src2, "");
   LLVMValueRef result = lp_build_mod(int_bld, src, divisor);
   return LLVMBuildOr(builder, div_mask, result, "");
}

static LLVMValueRef do_alu_action(struct lp_build_nir_context *bld_base,
                                  nir_op op, unsigned src_bit_size[NIR_MAX_VEC_COMPONENTS], LLVMValueRef src[NIR_MAX_VEC_COMPONENTS])
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef result;
   switch (op) {
   case nir_op_b2f32:
      result = emit_b2f(bld_base, src[0], 32);
      break;
   case nir_op_b2f64:
      result = emit_b2f(bld_base, src[0], 64);
      break;
   case nir_op_b2i32:
      result = emit_b2i(bld_base, src[0], 32);
      break;
   case nir_op_b2i64:
      result = emit_b2i(bld_base, src[0], 64);
      break;
   case nir_op_b32csel:
      result = emit_b32csel(bld_base, src_bit_size, src);
      break;
   case nir_op_bit_count:
      result = lp_build_popcount(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_bitfield_select:
      result = lp_build_xor(&bld_base->uint_bld, src[2], lp_build_and(&bld_base->uint_bld, src[0], lp_build_xor(&bld_base->uint_bld, src[1], src[2])));
      break;
   case nir_op_bitfield_reverse:
      result = lp_build_bitfield_reverse(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_f2b32:
      result = flt_to_bool32(bld_base, src_bit_size[0], src[0]);
      break;
   case nir_op_f2f32:
      result = LLVMBuildFPTrunc(builder, src[0],
                                bld_base->base.vec_type, "");
      break;
   case nir_op_f2f64:
      result = LLVMBuildFPExt(builder, src[0],
                              bld_base->dbl_bld.vec_type, "");
      break;
   case nir_op_f2i32:
      result = LLVMBuildFPToSI(builder, src[0], bld_base->base.int_vec_type, "");
      break;
   case nir_op_f2u32:
      result = LLVMBuildFPToUI(builder,
                               src[0],
                               bld_base->base.int_vec_type, "");
      break;
   case nir_op_f2i64:
      result = LLVMBuildFPToSI(builder,
                               src[0],
                               bld_base->int64_bld.vec_type, "");
      break;
   case nir_op_f2u64:
      result = LLVMBuildFPToUI(builder,
                               src[0],
                               bld_base->uint64_bld.vec_type, "");
      break;
   case nir_op_fabs:
      result = lp_build_abs(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fadd:
      result = lp_build_add(get_flt_bld(bld_base, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_fceil:
      result = lp_build_ceil(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fcos:
      result = lp_build_cos(&bld_base->base, src[0]);
      break;
   case nir_op_fddx:
   case nir_op_fddx_coarse:
   case nir_op_fddx_fine:
      result = lp_build_ddx(&bld_base->base, src[0]);
      break;
   case nir_op_fddy:
   case nir_op_fddy_coarse:
   case nir_op_fddy_fine:
      result = lp_build_ddy(&bld_base->base, src[0]);
      break;
   case nir_op_fdiv:
      result = lp_build_div(get_flt_bld(bld_base, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_feq32:
      result = fcmp32(bld_base, PIPE_FUNC_EQUAL, src_bit_size[0], src);
      break;
   case nir_op_fexp2:
      result = lp_build_exp2(&bld_base->base, src[0]);
      break;
   case nir_op_ffloor:
      result = lp_build_floor(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_ffma:
      result = lp_build_fmuladd(builder, src[0], src[1], src[2]);
      break;
   case nir_op_ffract: {
      struct lp_build_context *flt_bld = get_flt_bld(bld_base, src_bit_size[0]);
      LLVMValueRef tmp = lp_build_floor(flt_bld, src[0]);
      result = lp_build_sub(flt_bld, src[0], tmp);
      break;
   }
   case nir_op_fge32:
      result = fcmp32(bld_base, PIPE_FUNC_GEQUAL, src_bit_size[0], src);
      break;
   case nir_op_find_lsb:
      result = lp_build_cttz(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_flog2:
      result = lp_build_log2_safe(&bld_base->base, src[0]);
      break;
   case nir_op_flt32:
      result = fcmp32(bld_base, PIPE_FUNC_LESS, src_bit_size[0], src);
      break;
   case nir_op_fmin:
      result = lp_build_min(get_flt_bld(bld_base, src_bit_size[0]), src[0], src[1]);
      break;
   case nir_op_fmod: {
      struct lp_build_context *flt_bld = get_flt_bld(bld_base, src_bit_size[0]);
      result = lp_build_div(flt_bld, src[0], src[1]);
      result = lp_build_floor(flt_bld, result);
      result = lp_build_mul(flt_bld, src[1], result);
      result = lp_build_sub(flt_bld, src[0], result);
      break;
   }
   case nir_op_fmul:
      result = lp_build_mul(get_flt_bld(bld_base, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_fmax:
      result = lp_build_max(get_flt_bld(bld_base, src_bit_size[0]), src[0], src[1]);
      break;
   case nir_op_fne32:
      result = fcmp32(bld_base, PIPE_FUNC_NOTEQUAL, src_bit_size[0], src);
      break;
   case nir_op_fneg:
      result = lp_build_negate(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fpow:
      result = lp_build_pow(&bld_base->base, src[0], src[1]);
      break;
   case nir_op_frcp:
      result = lp_build_rcp(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fround_even:
      result = lp_build_round(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_frsq:
      result = lp_build_rsqrt(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fsat:
      result = lp_build_clamp_zero_one_nanzero(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fsign:
      result = lp_build_sgn(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_fsin:
      result = lp_build_sin(&bld_base->base, src[0]);
      break;
   case nir_op_fsqrt:
      result = lp_build_sqrt(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_ftrunc:
      result = lp_build_trunc(get_flt_bld(bld_base, src_bit_size[0]), src[0]);
      break;
   case nir_op_i2b32:
      result = int_to_bool32(bld_base, src_bit_size[0], false, src[0]);
      break;
   case nir_op_i2f32:
      result = lp_build_int_to_float(&bld_base->base, src[0]);
      break;
   case nir_op_i2f64:
      result = lp_build_int_to_float(&bld_base->dbl_bld, src[0]);
      break;
   case nir_op_i2i8:
      result = LLVMBuildTrunc(builder, src[0], bld_base->int8_bld.vec_type, "");
      break;
   case nir_op_i2i16:
      if (src_bit_size[0] < 16)
         result = LLVMBuildSExt(builder, src[0], bld_base->int16_bld.vec_type, "");
      else
         result = LLVMBuildTrunc(builder, src[0], bld_base->int16_bld.vec_type, "");
      break;
   case nir_op_i2i32:
      if (src_bit_size[0] < 32)
         result = LLVMBuildSExt(builder, src[0], bld_base->int_bld.vec_type, "");
      else
         result = LLVMBuildTrunc(builder, src[0], bld_base->int_bld.vec_type, "");
      break;
   case nir_op_i2i64:
      result = LLVMBuildSExt(builder, src[0], bld_base->int64_bld.vec_type, "");
      break;
   case nir_op_iabs:
      result = lp_build_abs(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_iadd:
      result = lp_build_add(get_int_bld(bld_base, false, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_iand:
      result = lp_build_and(get_int_bld(bld_base, false, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_idiv:
      result = do_int_divide(bld_base, false, src_bit_size[0], src[0], src[1]);
      break;
   case nir_op_ieq32:
      result = icmp32(bld_base, PIPE_FUNC_EQUAL, false, src_bit_size[0], src);
      break;
   case nir_op_ige32:
      result = icmp32(bld_base, PIPE_FUNC_GEQUAL, false, src_bit_size[0], src);
      break;
   case nir_op_ilt32:
      result = icmp32(bld_base, PIPE_FUNC_LESS, false, src_bit_size[0], src);
      break;
   case nir_op_imax:
      result = lp_build_max(get_int_bld(bld_base, false, src_bit_size[0]), src[0], src[1]);
      break;
   case nir_op_imin:
      result = lp_build_min(get_int_bld(bld_base, false, src_bit_size[0]), src[0], src[1]);
      break;
   case nir_op_imul:
   case nir_op_imul24:
      result = lp_build_mul(get_int_bld(bld_base, false, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_imul_high: {
      LLVMValueRef hi_bits;
      lp_build_mul_32_lohi(&bld_base->int_bld, src[0], src[1], &hi_bits);
      result = hi_bits;
      break;
   }
   case nir_op_ine32:
      result = icmp32(bld_base, PIPE_FUNC_NOTEQUAL, false, src_bit_size[0], src);
      break;
   case nir_op_ineg:
      result = lp_build_negate(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_inot:
      result = lp_build_not(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_ior:
      result = lp_build_or(get_int_bld(bld_base, false, src_bit_size[0]),
                           src[0], src[1]);
      break;
   case nir_op_irem:
      result = do_int_mod(bld_base, false, src_bit_size[0], src[0], src[1]);
      break;
   case nir_op_ishl: {
      struct lp_build_context *uint_bld = get_int_bld(bld_base, true, src_bit_size[0]);
      struct lp_build_context *int_bld = get_int_bld(bld_base, false, src_bit_size[0]);
      if (src_bit_size[0] == 64)
         src[1] = LLVMBuildZExt(builder, src[1], uint_bld->vec_type, "");
      if (src_bit_size[0] < 32)
         src[1] = LLVMBuildTrunc(builder, src[1], uint_bld->vec_type, "");
      src[1] = lp_build_and(uint_bld, src[1], lp_build_const_int_vec(gallivm, uint_bld->type, (src_bit_size[0] - 1)));
      result = lp_build_shl(int_bld, src[0], src[1]);
      break;
   }
   case nir_op_ishr: {
      struct lp_build_context *uint_bld = get_int_bld(bld_base, true, src_bit_size[0]);
      struct lp_build_context *int_bld = get_int_bld(bld_base, false, src_bit_size[0]);
      if (src_bit_size[0] == 64)
         src[1] = LLVMBuildZExt(builder, src[1], uint_bld->vec_type, "");
      if (src_bit_size[0] < 32)
         src[1] = LLVMBuildTrunc(builder, src[1], uint_bld->vec_type, "");
      src[1] = lp_build_and(uint_bld, src[1], lp_build_const_int_vec(gallivm, uint_bld->type, (src_bit_size[0] - 1)));
      result = lp_build_shr(int_bld, src[0], src[1]);
      break;
   }
   case nir_op_isign:
      result = lp_build_sgn(get_int_bld(bld_base, false, src_bit_size[0]), src[0]);
      break;
   case nir_op_isub:
      result = lp_build_sub(get_int_bld(bld_base, false, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_ixor:
      result = lp_build_xor(get_int_bld(bld_base, false, src_bit_size[0]),
                            src[0], src[1]);
      break;
   case nir_op_mov:
      result = src[0];
      break;
   case nir_op_unpack_64_2x32_split_x:
      result = split_64bit(bld_base, src[0], false);
      break;
   case nir_op_unpack_64_2x32_split_y:
      result = split_64bit(bld_base, src[0], true);
      break;

   case nir_op_pack_64_2x32_split: {
      LLVMValueRef tmp = merge_64bit(bld_base, src[0], src[1]);
      result = LLVMBuildBitCast(builder, tmp, bld_base->dbl_bld.vec_type, "");
      break;
   }
   case nir_op_u2f32:
      result = LLVMBuildUIToFP(builder, src[0], bld_base->base.vec_type, "");
      break;
   case nir_op_u2f64:
      result = LLVMBuildUIToFP(builder, src[0], bld_base->dbl_bld.vec_type, "");
      break;
   case nir_op_u2u8:
      result = LLVMBuildTrunc(builder, src[0], bld_base->uint8_bld.vec_type, "");
      break;
   case nir_op_u2u16:
      if (src_bit_size[0] < 16)
         result = LLVMBuildZExt(builder, src[0], bld_base->uint16_bld.vec_type, "");
      else
         result = LLVMBuildTrunc(builder, src[0], bld_base->uint16_bld.vec_type, "");
      break;
   case nir_op_u2u32:
      if (src_bit_size[0] < 32)
         result = LLVMBuildZExt(builder, src[0], bld_base->uint_bld.vec_type, "");
      else
         result = LLVMBuildTrunc(builder, src[0], bld_base->uint_bld.vec_type, "");
      break;
   case nir_op_u2u64:
      result = LLVMBuildZExt(builder, src[0], bld_base->uint64_bld.vec_type, "");
      break;
   case nir_op_udiv:
      result = do_int_divide(bld_base, true, src_bit_size[0], src[0], src[1]);
      break;
   case nir_op_ufind_msb: {
      struct lp_build_context *uint_bld = get_int_bld(bld_base, true, src_bit_size[0]);
      result = lp_build_ctlz(uint_bld, src[0]);
      result = lp_build_sub(uint_bld, lp_build_const_int_vec(gallivm, uint_bld->type, src_bit_size[0] - 1), result);
      break;
   }
   case nir_op_uge32:
      result = icmp32(bld_base, PIPE_FUNC_GEQUAL, true, src_bit_size[0], src);
      break;
   case nir_op_ult32:
      result = icmp32(bld_base, PIPE_FUNC_LESS, true, src_bit_size[0], src);
      break;
   case nir_op_umax:
      result = lp_build_max(get_int_bld(bld_base, true, src_bit_size[0]), src[0], src[1]);
      break;
   case nir_op_umin:
      result = lp_build_min(get_int_bld(bld_base, true, src_bit_size[0]), src[0], src[1]);
      break;
   case nir_op_umod:
      result = do_int_mod(bld_base, true, src_bit_size[0], src[0], src[1]);
      break;
   case nir_op_umul_high: {
      LLVMValueRef hi_bits;
      lp_build_mul_32_lohi(&bld_base->uint_bld, src[0], src[1], &hi_bits);
      result = hi_bits;
      break;
   }
   case nir_op_ushr: {
      struct lp_build_context *uint_bld = get_int_bld(bld_base, true, src_bit_size[0]);
      if (src_bit_size[0] == 64)
         src[1] = LLVMBuildZExt(builder, src[1], uint_bld->vec_type, "");
      if (src_bit_size[0] < 32)
         src[1] = LLVMBuildTrunc(builder, src[1], uint_bld->vec_type, "");
      src[1] = lp_build_and(uint_bld, src[1], lp_build_const_int_vec(gallivm, uint_bld->type, (src_bit_size[0] - 1)));
      result = lp_build_shr(uint_bld, src[0], src[1]);
      break;
   }
   default:
      assert(0);
      break;
   }
   return result;
}

static void visit_alu(struct lp_build_nir_context *bld_base, const nir_alu_instr *instr)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMValueRef src[NIR_MAX_VEC_COMPONENTS];
   unsigned src_bit_size[NIR_MAX_VEC_COMPONENTS];
   unsigned num_components = nir_dest_num_components(instr->dest.dest);
   unsigned src_components;
   switch (instr->op) {
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_vec8:
   case nir_op_vec16:
      src_components = 1;
      break;
   case nir_op_pack_half_2x16:
      src_components = 2;
      break;
   case nir_op_unpack_half_2x16:
      src_components = 1;
      break;
   case nir_op_cube_face_coord:
   case nir_op_cube_face_index:
      src_components = 3;
      break;
   default:
      src_components = num_components;
      break;
   }
   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      src[i] = get_alu_src(bld_base, instr->src[i], src_components);
      src_bit_size[i] = nir_src_bit_size(instr->src[i].src);
   }

   LLVMValueRef result[NIR_MAX_VEC_COMPONENTS];
   if (instr->op == nir_op_vec4 || instr->op == nir_op_vec3 || instr->op == nir_op_vec2 || instr->op == nir_op_vec8 || instr->op == nir_op_vec16) {
      for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
         result[i] = cast_type(bld_base, src[i], nir_op_infos[instr->op].input_types[i], src_bit_size[i]);
      }
   } else {
      for (unsigned c = 0; c < num_components; c++) {
         LLVMValueRef src_chan[NIR_MAX_VEC_COMPONENTS];

         for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
            if (num_components > 1) {
               src_chan[i] = LLVMBuildExtractValue(gallivm->builder,
                                                     src[i], c, "");
            } else
               src_chan[i] = src[i];
            src_chan[i] = cast_type(bld_base, src_chan[i], nir_op_infos[instr->op].input_types[i], src_bit_size[i]);
         }
         result[c] = do_alu_action(bld_base, instr->op, src_bit_size, src_chan);
         result[c] = cast_type(bld_base, result[c], nir_op_infos[instr->op].output_type, nir_dest_bit_size(instr->dest.dest));
      }
   }
   assign_alu_dest(bld_base, &instr->dest, result);
 }

static void visit_load_const(struct lp_build_nir_context *bld_base,
                             const nir_load_const_instr *instr)
{
   LLVMValueRef result[NIR_MAX_VEC_COMPONENTS];
   struct lp_build_context *int_bld = get_int_bld(bld_base, true, instr->def.bit_size);
   for (unsigned i = 0; i < instr->def.num_components; i++)
      result[i] = lp_build_const_int_vec(bld_base->base.gallivm, int_bld->type, instr->value[i].u64);
   assign_ssa_dest(bld_base, &instr->def, result);
}

static void
get_deref_offset(struct lp_build_nir_context *bld_base, nir_deref_instr *instr,
                 bool vs_in, unsigned *vertex_index_out,
                 LLVMValueRef *vertex_index_ref,
                 unsigned *const_out, LLVMValueRef *indir_out)
{
   LLVMBuilderRef builder = bld_base->base.gallivm->builder;
   nir_variable *var = nir_deref_instr_get_variable(instr);
   nir_deref_path path;
   unsigned idx_lvl = 1;

   nir_deref_path_init(&path, instr, NULL);

   if (vertex_index_out != NULL || vertex_index_ref != NULL) {
      if (vertex_index_ref) {
         *vertex_index_ref = get_src(bld_base, path.path[idx_lvl]->arr.index);
         if (vertex_index_out)
            *vertex_index_out = 0;
      } else {
         *vertex_index_out = nir_src_as_uint(path.path[idx_lvl]->arr.index);
      }
      ++idx_lvl;
   }

   uint32_t const_offset = 0;
   LLVMValueRef offset = NULL;

   if (var->data.compact) {
      assert(instr->deref_type == nir_deref_type_array);
      const_offset = nir_src_as_uint(instr->arr.index);
      goto out;
   }

   for (; path.path[idx_lvl]; ++idx_lvl) {
      const struct glsl_type *parent_type = path.path[idx_lvl - 1]->type;
      if (path.path[idx_lvl]->deref_type == nir_deref_type_struct) {
         unsigned index = path.path[idx_lvl]->strct.index;

         for (unsigned i = 0; i < index; i++) {
            const struct glsl_type *ft = glsl_get_struct_field(parent_type, i);
            const_offset += glsl_count_attribute_slots(ft, vs_in);
         }
      } else if(path.path[idx_lvl]->deref_type == nir_deref_type_array) {
         unsigned size = glsl_count_attribute_slots(path.path[idx_lvl]->type, vs_in);
         if (nir_src_is_const(path.path[idx_lvl]->arr.index)) {
           const_offset += nir_src_comp_as_int(path.path[idx_lvl]->arr.index, 0) * size;
         } else {
           LLVMValueRef idx_src = get_src(bld_base, path.path[idx_lvl]->arr.index);
           idx_src = cast_type(bld_base, idx_src, nir_type_uint, 32);
           LLVMValueRef array_off = lp_build_mul(&bld_base->uint_bld, lp_build_const_int_vec(bld_base->base.gallivm, bld_base->base.type, size),
                                               idx_src);
           if (offset)
             offset = lp_build_add(&bld_base->uint_bld, offset, array_off);
           else
             offset = array_off;
         }
      } else
         unreachable("Uhandled deref type in get_deref_instr_offset");
   }

out:
   nir_deref_path_finish(&path);

   if (const_offset && offset)
      offset = LLVMBuildAdd(builder, offset,
                            lp_build_const_int_vec(bld_base->base.gallivm, bld_base->uint_bld.type, const_offset),
                            "");
   *const_out = const_offset;
   *indir_out = offset;
}

static void visit_load_var(struct lp_build_nir_context *bld_base,
                           nir_intrinsic_instr *instr,
                           LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   nir_deref_instr *deref = nir_instr_as_deref(instr->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   nir_variable_mode mode = deref->mode;
   unsigned const_index;
   LLVMValueRef indir_index;
   LLVMValueRef indir_vertex_index = NULL;
   unsigned vertex_index = 0;
   unsigned nc = nir_dest_num_components(instr->dest);
   unsigned bit_size = nir_dest_bit_size(instr->dest);
   if (var) {
      bool vs_in = bld_base->shader->info.stage == MESA_SHADER_VERTEX &&
         var->data.mode == nir_var_shader_in;
      bool gs_in = bld_base->shader->info.stage == MESA_SHADER_GEOMETRY &&
         var->data.mode == nir_var_shader_in;
      bool tcs_in = bld_base->shader->info.stage == MESA_SHADER_TESS_CTRL &&
         var->data.mode == nir_var_shader_in;
      bool tcs_out = bld_base->shader->info.stage == MESA_SHADER_TESS_CTRL &&
         var->data.mode == nir_var_shader_out && !var->data.patch;
      bool tes_in = bld_base->shader->info.stage == MESA_SHADER_TESS_EVAL &&
         var->data.mode == nir_var_shader_in && !var->data.patch;

      mode = var->data.mode;

      get_deref_offset(bld_base, deref, vs_in, gs_in ? &vertex_index : NULL, (tcs_in || tcs_out || tes_in) ? &indir_vertex_index : NULL,
                       &const_index, &indir_index);
   }
   bld_base->load_var(bld_base, mode, nc, bit_size, var, vertex_index, indir_vertex_index, const_index, indir_index, result);
}

static void
visit_store_var(struct lp_build_nir_context *bld_base,
                nir_intrinsic_instr *instr)
{
   nir_deref_instr *deref = nir_instr_as_deref(instr->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   nir_variable_mode mode = deref->mode;
   int writemask = instr->const_index[0];
   unsigned bit_size = nir_src_bit_size(instr->src[1]);
   LLVMValueRef src = get_src(bld_base, instr->src[1]);
   unsigned const_index = 0;
   LLVMValueRef indir_index, indir_vertex_index = NULL;
   if (var) {
      bool tcs_out = bld_base->shader->info.stage == MESA_SHADER_TESS_CTRL &&
         var->data.mode == nir_var_shader_out && !var->data.patch;
      get_deref_offset(bld_base, deref, false, NULL, tcs_out ? &indir_vertex_index : NULL,
                       &const_index, &indir_index);
   }
   bld_base->store_var(bld_base, mode, instr->num_components, bit_size, var, writemask, indir_vertex_index, const_index, indir_index, src);
}

static void visit_load_ubo(struct lp_build_nir_context *bld_base,
                           nir_intrinsic_instr *instr,
                           LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef idx = get_src(bld_base, instr->src[0]);
   LLVMValueRef offset = get_src(bld_base, instr->src[1]);

   bool offset_is_uniform = nir_src_is_dynamically_uniform(instr->src[1]);
   idx = LLVMBuildExtractElement(builder, idx, lp_build_const_int32(gallivm, 0), "");
   bld_base->load_ubo(bld_base, nir_dest_num_components(instr->dest), nir_dest_bit_size(instr->dest),
                      offset_is_uniform, idx, offset, result);
}


static void visit_load_ssbo(struct lp_build_nir_context *bld_base,
                           nir_intrinsic_instr *instr,
                           LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef idx = get_src(bld_base, instr->src[0]);
   LLVMValueRef offset = get_src(bld_base, instr->src[1]);
   bld_base->load_mem(bld_base, nir_dest_num_components(instr->dest), nir_dest_bit_size(instr->dest),
                       idx, offset, result);
}

static void visit_store_ssbo(struct lp_build_nir_context *bld_base,
                             nir_intrinsic_instr *instr)
{
   LLVMValueRef val = get_src(bld_base, instr->src[0]);
   LLVMValueRef idx = get_src(bld_base, instr->src[1]);
   LLVMValueRef offset = get_src(bld_base, instr->src[2]);
   int writemask = instr->const_index[0];
   int nc = nir_src_num_components(instr->src[0]);
   int bitsize = nir_src_bit_size(instr->src[0]);
   bld_base->store_mem(bld_base, writemask, nc, bitsize, idx, offset, val);
}

static void visit_get_buffer_size(struct lp_build_nir_context *bld_base,
                                  nir_intrinsic_instr *instr,
                                  LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef idx = get_src(bld_base, instr->src[0]);
   result[0] = bld_base->get_buffer_size(bld_base, idx);
}

static void visit_ssbo_atomic(struct lp_build_nir_context *bld_base,
                              nir_intrinsic_instr *instr,
                              LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef idx = get_src(bld_base, instr->src[0]);
   LLVMValueRef offset = get_src(bld_base, instr->src[1]);
   LLVMValueRef val = get_src(bld_base, instr->src[2]);
   LLVMValueRef val2 = NULL;
   if (instr->intrinsic == nir_intrinsic_ssbo_atomic_comp_swap)
      val2 = get_src(bld_base, instr->src[3]);

   bld_base->atomic_mem(bld_base, instr->intrinsic, idx, offset, val, val2, &result[0]);

}

static void visit_load_image(struct lp_build_nir_context *bld_base,
                             nir_intrinsic_instr *instr,
                             LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   nir_deref_instr *deref = nir_instr_as_deref(instr->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   LLVMValueRef coord_val = get_src(bld_base, instr->src[1]);
   LLVMValueRef coords[5];
   struct lp_img_params params;
   const struct glsl_type *type = glsl_without_array(var->type);

   memset(&params, 0, sizeof(params));
   params.target = glsl_sampler_to_pipe(glsl_get_sampler_dim(type), glsl_sampler_type_is_array(type));
   for (unsigned i = 0; i < 4; i++)
      coords[i] = LLVMBuildExtractValue(builder, coord_val, i, "");
   if (params.target == PIPE_TEXTURE_1D_ARRAY)
      coords[2] = coords[1];

   params.coords = coords;
   params.outdata = result;
   params.img_op = LP_IMG_LOAD;
   params.image_index = var->data.binding;
   bld_base->image_op(bld_base, &params);
}

static void visit_store_image(struct lp_build_nir_context *bld_base,
                              nir_intrinsic_instr *instr)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   nir_deref_instr *deref = nir_instr_as_deref(instr->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   LLVMValueRef coord_val = get_src(bld_base, instr->src[1]);
   LLVMValueRef in_val = get_src(bld_base, instr->src[3]);
   LLVMValueRef coords[5];
   struct lp_img_params params;
   const struct glsl_type *type = glsl_without_array(var->type);

   memset(&params, 0, sizeof(params));
   params.target = glsl_sampler_to_pipe(glsl_get_sampler_dim(type), glsl_sampler_type_is_array(type));
   for (unsigned i = 0; i < 4; i++)
      coords[i] = LLVMBuildExtractValue(builder, coord_val, i, "");
   if (params.target == PIPE_TEXTURE_1D_ARRAY)
      coords[2] = coords[1];
   params.coords = coords;

   for (unsigned i = 0; i < 4; i++) {
      params.indata[i] = LLVMBuildExtractValue(builder, in_val, i, "");
      params.indata[i] = LLVMBuildBitCast(builder, params.indata[i], bld_base->base.vec_type, "");
   }
   params.img_op = LP_IMG_STORE;
   params.image_index = var->data.binding;

   if (params.target == PIPE_TEXTURE_1D_ARRAY)
      coords[2] = coords[1];
   bld_base->image_op(bld_base, &params);
}

static void visit_atomic_image(struct lp_build_nir_context *bld_base,
                               nir_intrinsic_instr *instr,
                               LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   nir_deref_instr *deref = nir_instr_as_deref(instr->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   struct lp_img_params params;
   LLVMValueRef coord_val = get_src(bld_base, instr->src[1]);
   LLVMValueRef in_val = get_src(bld_base, instr->src[3]);
   LLVMValueRef coords[5];
   const struct glsl_type *type = glsl_without_array(var->type);

   memset(&params, 0, sizeof(params));

   switch (instr->intrinsic) {
   case nir_intrinsic_image_deref_atomic_add:
      params.op = LLVMAtomicRMWBinOpAdd;
      break;
   case nir_intrinsic_image_deref_atomic_exchange:
      params.op = LLVMAtomicRMWBinOpXchg;
      break;
   case nir_intrinsic_image_deref_atomic_and:
      params.op = LLVMAtomicRMWBinOpAnd;
      break;
   case nir_intrinsic_image_deref_atomic_or:
      params.op = LLVMAtomicRMWBinOpOr;
      break;
   case nir_intrinsic_image_deref_atomic_xor:
      params.op = LLVMAtomicRMWBinOpXor;
      break;
   case nir_intrinsic_image_deref_atomic_umin:
      params.op = LLVMAtomicRMWBinOpUMin;
      break;
   case nir_intrinsic_image_deref_atomic_umax:
      params.op = LLVMAtomicRMWBinOpUMax;
      break;
   case nir_intrinsic_image_deref_atomic_imin:
      params.op = LLVMAtomicRMWBinOpMin;
      break;
   case nir_intrinsic_image_deref_atomic_imax:
      params.op = LLVMAtomicRMWBinOpMax;
      break;
   default:
      break;
   }

   params.target = glsl_sampler_to_pipe(glsl_get_sampler_dim(type), glsl_sampler_type_is_array(type));
   for (unsigned i = 0; i < 4; i++)
      coords[i] = LLVMBuildExtractValue(builder, coord_val, i, "");
   if (params.target == PIPE_TEXTURE_1D_ARRAY)
      coords[2] = coords[1];
   params.coords = coords;
   if (instr->intrinsic == nir_intrinsic_image_deref_atomic_comp_swap) {
      LLVMValueRef cas_val = get_src(bld_base, instr->src[4]);
      params.indata[0] = in_val;
      params.indata2[0] = cas_val;
   } else
      params.indata[0] = in_val;

   params.outdata = result;
   params.img_op = (instr->intrinsic == nir_intrinsic_image_deref_atomic_comp_swap) ? LP_IMG_ATOMIC_CAS : LP_IMG_ATOMIC;
   params.image_index = var->data.binding;

   bld_base->image_op(bld_base, &params);
}


static void visit_image_size(struct lp_build_nir_context *bld_base,
                             nir_intrinsic_instr *instr,
                             LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   nir_deref_instr *deref = nir_instr_as_deref(instr->src[0].ssa->parent_instr);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   struct lp_sampler_size_query_params params = { 0 };
   params.texture_unit = var->data.binding;
   params.target = glsl_sampler_to_pipe(glsl_get_sampler_dim(var->type), glsl_sampler_type_is_array(var->type));
   params.sizes_out = result;

   bld_base->image_size(bld_base, &params);
}

static void visit_shared_load(struct lp_build_nir_context *bld_base,
                                nir_intrinsic_instr *instr,
                                LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef offset = get_src(bld_base, instr->src[0]);
   bld_base->load_mem(bld_base, nir_dest_num_components(instr->dest), nir_dest_bit_size(instr->dest),
                      NULL, offset, result);
}

static void visit_shared_store(struct lp_build_nir_context *bld_base,
                               nir_intrinsic_instr *instr)
{
   LLVMValueRef val = get_src(bld_base, instr->src[0]);
   LLVMValueRef offset = get_src(bld_base, instr->src[1]);
   int writemask = instr->const_index[1];
   int nc = nir_src_num_components(instr->src[0]);
   int bitsize = nir_src_bit_size(instr->src[0]);
   bld_base->store_mem(bld_base, writemask, nc, bitsize, NULL, offset, val);
}

static void visit_shared_atomic(struct lp_build_nir_context *bld_base,
                                nir_intrinsic_instr *instr,
                                LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef offset = get_src(bld_base, instr->src[0]);
   LLVMValueRef val = get_src(bld_base, instr->src[1]);
   LLVMValueRef val2 = NULL;
   if (instr->intrinsic == nir_intrinsic_shared_atomic_comp_swap)
      val2 = get_src(bld_base, instr->src[2]);

   bld_base->atomic_mem(bld_base, instr->intrinsic, NULL, offset, val, val2, &result[0]);

}

static void visit_barrier(struct lp_build_nir_context *bld_base)
{
   bld_base->barrier(bld_base);
}

static void visit_discard(struct lp_build_nir_context *bld_base,
                          nir_intrinsic_instr *instr)
{
   LLVMValueRef cond = NULL;
   if (instr->intrinsic == nir_intrinsic_discard_if) {
      cond = get_src(bld_base, instr->src[0]);
      cond = cast_type(bld_base, cond, nir_type_int, 32);
   }
   bld_base->discard(bld_base, cond);
}

static void visit_load_kernel_input(struct lp_build_nir_context *bld_base,
                                    nir_intrinsic_instr *instr, LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef offset = get_src(bld_base, instr->src[0]);

   bool offset_is_uniform = nir_src_is_dynamically_uniform(instr->src[0]);
   bld_base->load_kernel_arg(bld_base, nir_dest_num_components(instr->dest), nir_dest_bit_size(instr->dest),
                             nir_src_bit_size(instr->src[0]),
                             offset_is_uniform, offset, result);
}

static void visit_load_global(struct lp_build_nir_context *bld_base,
                              nir_intrinsic_instr *instr, LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef addr = get_src(bld_base, instr->src[0]);
   bld_base->load_global(bld_base, nir_dest_num_components(instr->dest), nir_dest_bit_size(instr->dest),
                         nir_src_bit_size(instr->src[0]),
                         addr, result);
}

static void visit_store_global(struct lp_build_nir_context *bld_base,
                               nir_intrinsic_instr *instr)
{
   LLVMValueRef val = get_src(bld_base, instr->src[0]);
   int nc = nir_src_num_components(instr->src[0]);
   int bitsize = nir_src_bit_size(instr->src[0]);
   LLVMValueRef addr = get_src(bld_base, instr->src[1]);
   int addr_bitsize = nir_src_bit_size(instr->src[1]);
   int writemask = instr->const_index[0];
   bld_base->store_global(bld_base, writemask, nc, bitsize, addr_bitsize, addr, val);
}

static void visit_global_atomic(struct lp_build_nir_context *bld_base,
                                nir_intrinsic_instr *instr,
                                LLVMValueRef result[NIR_MAX_VEC_COMPONENTS])
{
   LLVMValueRef addr = get_src(bld_base, instr->src[0]);
   LLVMValueRef val = get_src(bld_base, instr->src[1]);
   LLVMValueRef val2 = NULL;
   int addr_bitsize = nir_src_bit_size(instr->src[0]);
   if (instr->intrinsic == nir_intrinsic_global_atomic_comp_swap)
      val2 = get_src(bld_base, instr->src[2]);

   bld_base->atomic_global(bld_base, instr->intrinsic, addr_bitsize, addr, val, val2, &result[0]);
}

static void visit_intrinsic(struct lp_build_nir_context *bld_base,
                            nir_intrinsic_instr *instr)
{
   LLVMValueRef result[NIR_MAX_VEC_COMPONENTS] = {0};
   switch (instr->intrinsic) {
   case nir_intrinsic_load_deref:
      visit_load_var(bld_base, instr, result);
      break;
   case nir_intrinsic_store_deref:
      visit_store_var(bld_base, instr);
      break;
   case nir_intrinsic_load_ubo:
      visit_load_ubo(bld_base, instr, result);
      break;
   case nir_intrinsic_load_ssbo:
      visit_load_ssbo(bld_base, instr, result);
      break;
   case nir_intrinsic_store_ssbo:
      visit_store_ssbo(bld_base, instr);
      break;
   case nir_intrinsic_get_buffer_size:
      visit_get_buffer_size(bld_base, instr, result);
      break;
   case nir_intrinsic_load_vertex_id:
   case nir_intrinsic_load_primitive_id:
   case nir_intrinsic_load_instance_id:
   case nir_intrinsic_load_base_instance:
   case nir_intrinsic_load_base_vertex:
   case nir_intrinsic_load_work_group_id:
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_num_work_groups:
   case nir_intrinsic_load_invocation_id:
   case nir_intrinsic_load_front_face:
   case nir_intrinsic_load_draw_id:
   case nir_intrinsic_load_local_group_size:
   case nir_intrinsic_load_work_dim:
   case nir_intrinsic_load_tess_coord:
   case nir_intrinsic_load_tess_level_outer:
   case nir_intrinsic_load_tess_level_inner:
   case nir_intrinsic_load_patch_vertices_in:
      bld_base->sysval_intrin(bld_base, instr, result);
      break;
   case nir_intrinsic_discard_if:
   case nir_intrinsic_discard:
      visit_discard(bld_base, instr);
      break;
   case nir_intrinsic_emit_vertex:
      bld_base->emit_vertex(bld_base, nir_intrinsic_stream_id(instr));
      break;
   case nir_intrinsic_end_primitive:
      bld_base->end_primitive(bld_base, nir_intrinsic_stream_id(instr));
      break;
   case nir_intrinsic_ssbo_atomic_add:
   case nir_intrinsic_ssbo_atomic_imin:
   case nir_intrinsic_ssbo_atomic_imax:
   case nir_intrinsic_ssbo_atomic_umin:
   case nir_intrinsic_ssbo_atomic_umax:
   case nir_intrinsic_ssbo_atomic_and:
   case nir_intrinsic_ssbo_atomic_or:
   case nir_intrinsic_ssbo_atomic_xor:
   case nir_intrinsic_ssbo_atomic_exchange:
   case nir_intrinsic_ssbo_atomic_comp_swap:
      visit_ssbo_atomic(bld_base, instr, result);
      break;
   case nir_intrinsic_image_deref_load:
      visit_load_image(bld_base, instr, result);
      break;
   case nir_intrinsic_image_deref_store:
      visit_store_image(bld_base, instr);
      break;
   case nir_intrinsic_image_deref_atomic_add:
   case nir_intrinsic_image_deref_atomic_imin:
   case nir_intrinsic_image_deref_atomic_imax:
   case nir_intrinsic_image_deref_atomic_umin:
   case nir_intrinsic_image_deref_atomic_umax:
   case nir_intrinsic_image_deref_atomic_and:
   case nir_intrinsic_image_deref_atomic_or:
   case nir_intrinsic_image_deref_atomic_xor:
   case nir_intrinsic_image_deref_atomic_exchange:
   case nir_intrinsic_image_deref_atomic_comp_swap:
      visit_atomic_image(bld_base, instr, result);
      break;
   case nir_intrinsic_image_deref_size:
      visit_image_size(bld_base, instr, result);
      break;
   case nir_intrinsic_load_shared:
      visit_shared_load(bld_base, instr, result);
      break;
   case nir_intrinsic_store_shared:
      visit_shared_store(bld_base, instr);
      break;
   case nir_intrinsic_shared_atomic_add:
   case nir_intrinsic_shared_atomic_imin:
   case nir_intrinsic_shared_atomic_umin:
   case nir_intrinsic_shared_atomic_imax:
   case nir_intrinsic_shared_atomic_umax:
   case nir_intrinsic_shared_atomic_and:
   case nir_intrinsic_shared_atomic_or:
   case nir_intrinsic_shared_atomic_xor:
   case nir_intrinsic_shared_atomic_exchange:
   case nir_intrinsic_shared_atomic_comp_swap:
      visit_shared_atomic(bld_base, instr, result);
      break;
   case nir_intrinsic_control_barrier:
      visit_barrier(bld_base);
      break;
   case nir_intrinsic_memory_barrier:
   case nir_intrinsic_memory_barrier_shared:
   case nir_intrinsic_memory_barrier_buffer:
   case nir_intrinsic_memory_barrier_image:
   case nir_intrinsic_memory_barrier_tcs_patch:
      break;
   case nir_intrinsic_load_kernel_input:
      visit_load_kernel_input(bld_base, instr, result);
     break;
   case nir_intrinsic_load_global:
      visit_load_global(bld_base, instr, result);
      break;
   case nir_intrinsic_store_global:
      visit_store_global(bld_base, instr);
      break;
   case nir_intrinsic_global_atomic_add:
   case nir_intrinsic_global_atomic_imin:
   case nir_intrinsic_global_atomic_umin:
   case nir_intrinsic_global_atomic_imax:
   case nir_intrinsic_global_atomic_umax:
   case nir_intrinsic_global_atomic_and:
   case nir_intrinsic_global_atomic_or:
   case nir_intrinsic_global_atomic_xor:
   case nir_intrinsic_global_atomic_exchange:
   case nir_intrinsic_global_atomic_comp_swap:
      visit_global_atomic(bld_base, instr, result);
   case nir_intrinsic_vote_all:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_ieq:
      bld_base->vote(bld_base, cast_type(bld_base, get_src(bld_base, instr->src[0]), nir_type_int, 32), instr, result);
      break;
   default:
      assert(0);
      break;
   }
   if (result[0]) {
      assign_dest(bld_base, &instr->dest, result);
   }
}

static void visit_txs(struct lp_build_nir_context *bld_base, nir_tex_instr *instr)
{
   struct lp_sampler_size_query_params params;
   LLVMValueRef sizes_out[NIR_MAX_VEC_COMPONENTS];
   LLVMValueRef explicit_lod = NULL;

   for (unsigned i = 0; i < instr->num_srcs; i++) {
      switch (instr->src[i].src_type) {
      case nir_tex_src_lod:
         explicit_lod = cast_type(bld_base, get_src(bld_base, instr->src[i].src), nir_type_int, 32);
         break;
      default:
         break;
      }
   }

   params.target = glsl_sampler_to_pipe(instr->sampler_dim, instr->is_array);
   params.texture_unit = instr->texture_index;
   params.explicit_lod = explicit_lod;
   params.is_sviewinfo = TRUE;
   params.sizes_out = sizes_out;

   if (instr->op == nir_texop_query_levels)
      params.explicit_lod = bld_base->uint_bld.zero;
   bld_base->tex_size(bld_base, &params);
   assign_dest(bld_base, &instr->dest, &sizes_out[instr->op == nir_texop_query_levels ? 3 : 0]);
}

static enum lp_sampler_lod_property lp_build_nir_lod_property(struct lp_build_nir_context *bld_base,
                                                              nir_src lod_src)
{
   enum lp_sampler_lod_property lod_property;

   if (nir_src_is_dynamically_uniform(lod_src))
      lod_property = LP_SAMPLER_LOD_SCALAR;
   else if (bld_base->shader->info.stage == MESA_SHADER_FRAGMENT) {
      if (gallivm_perf & GALLIVM_PERF_NO_QUAD_LOD)
         lod_property = LP_SAMPLER_LOD_PER_ELEMENT;
      else
         lod_property = LP_SAMPLER_LOD_PER_QUAD;
   }
   else
      lod_property = LP_SAMPLER_LOD_PER_ELEMENT;
   return lod_property;
}

static void visit_tex(struct lp_build_nir_context *bld_base, nir_tex_instr *instr)
{
   struct gallivm_state *gallivm = bld_base->base.gallivm;
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef coords[5];
   LLVMValueRef offsets[3] = { NULL };
   LLVMValueRef explicit_lod = NULL, projector = NULL;
   struct lp_sampler_params params;
   struct lp_derivatives derivs;
   unsigned sample_key = 0;
   nir_deref_instr *texture_deref_instr = NULL;
   nir_deref_instr *sampler_deref_instr = NULL;
   LLVMValueRef texel[NIR_MAX_VEC_COMPONENTS];
   unsigned lod_src = 0;
   LLVMValueRef coord_undef = LLVMGetUndef(bld_base->base.int_vec_type);

   memset(&params, 0, sizeof(params));
   enum lp_sampler_lod_property lod_property = LP_SAMPLER_LOD_SCALAR;

   if (instr->op == nir_texop_txs || instr->op == nir_texop_query_levels) {
      visit_txs(bld_base, instr);
      return;
   }
   if (instr->op == nir_texop_txf || instr->op == nir_texop_txf_ms)
      sample_key |= LP_SAMPLER_OP_FETCH << LP_SAMPLER_OP_TYPE_SHIFT;
   else if (instr->op == nir_texop_tg4) {
      sample_key |= LP_SAMPLER_OP_GATHER << LP_SAMPLER_OP_TYPE_SHIFT;
      sample_key |= (instr->component << LP_SAMPLER_GATHER_COMP_SHIFT);
   } else if (instr->op == nir_texop_lod)
      sample_key |= LP_SAMPLER_OP_LODQ << LP_SAMPLER_OP_TYPE_SHIFT;
   for (unsigned i = 0; i < instr->num_srcs; i++) {
      switch (instr->src[i].src_type) {
      case nir_tex_src_coord: {
         LLVMValueRef coord = get_src(bld_base, instr->src[i].src);
         if (instr->coord_components == 1)
            coords[0] = coord;
         else {
            for (unsigned chan = 0; chan < instr->coord_components; ++chan)
               coords[chan] = LLVMBuildExtractValue(builder, coord,
                                                    chan, "");
         }
         for (unsigned chan = instr->coord_components; chan < 5; chan++)
            coords[chan] = coord_undef;

         break;
      }
      case nir_tex_src_texture_deref:
         texture_deref_instr = nir_src_as_deref(instr->src[i].src);
         break;
      case nir_tex_src_sampler_deref:
         sampler_deref_instr = nir_src_as_deref(instr->src[i].src);
         break;
      case nir_tex_src_projector:
         projector = lp_build_rcp(&bld_base->base, cast_type(bld_base, get_src(bld_base, instr->src[i].src), nir_type_float, 32));
         break;
      case nir_tex_src_comparator:
         sample_key |= LP_SAMPLER_SHADOW;
         coords[4] = get_src(bld_base, instr->src[i].src);
         coords[4] = cast_type(bld_base, coords[4], nir_type_float, 32);
         break;
      case nir_tex_src_bias:
         sample_key |= LP_SAMPLER_LOD_BIAS << LP_SAMPLER_LOD_CONTROL_SHIFT;
         lod_src = i;
         explicit_lod = cast_type(bld_base, get_src(bld_base, instr->src[i].src), nir_type_float, 32);
         break;
      case nir_tex_src_lod:
         sample_key |= LP_SAMPLER_LOD_EXPLICIT << LP_SAMPLER_LOD_CONTROL_SHIFT;
         lod_src = i;
         if (instr->op == nir_texop_txf)
            explicit_lod = cast_type(bld_base, get_src(bld_base, instr->src[i].src), nir_type_int, 32);
         else
            explicit_lod = cast_type(bld_base, get_src(bld_base, instr->src[i].src), nir_type_float, 32);
         break;
      case nir_tex_src_ddx: {
         int deriv_cnt = instr->coord_components;
         if (instr->is_array)
            deriv_cnt--;
         LLVMValueRef deriv_val = get_src(bld_base, instr->src[i].src);
         if (deriv_cnt == 1)
            derivs.ddx[0] = deriv_val;
         else
            for (unsigned chan = 0; chan < deriv_cnt; ++chan)
               derivs.ddx[chan] = LLVMBuildExtractValue(builder, deriv_val,
                                                        chan, "");
         for (unsigned chan = 0; chan < deriv_cnt; ++chan)
            derivs.ddx[chan] = cast_type(bld_base, derivs.ddx[chan], nir_type_float, 32);
         break;
      }
      case nir_tex_src_ddy: {
         int deriv_cnt = instr->coord_components;
         if (instr->is_array)
            deriv_cnt--;
         LLVMValueRef deriv_val = get_src(bld_base, instr->src[i].src);
         if (deriv_cnt == 1)
            derivs.ddy[0] = deriv_val;
         else
            for (unsigned chan = 0; chan < deriv_cnt; ++chan)
               derivs.ddy[chan] = LLVMBuildExtractValue(builder, deriv_val,
                                                        chan, "");
         for (unsigned chan = 0; chan < deriv_cnt; ++chan)
            derivs.ddy[chan] = cast_type(bld_base, derivs.ddy[chan], nir_type_float, 32);
         break;
      }
      case nir_tex_src_offset: {
         int offset_cnt = instr->coord_components;
         if (instr->is_array)
            offset_cnt--;
         LLVMValueRef offset_val = get_src(bld_base, instr->src[i].src);
         sample_key |= LP_SAMPLER_OFFSETS;
         if (offset_cnt == 1)
            offsets[0] = cast_type(bld_base, offset_val, nir_type_int, 32);
         else {
            for (unsigned chan = 0; chan < offset_cnt; ++chan) {
               offsets[chan] = LLVMBuildExtractValue(builder, offset_val,
                                                     chan, "");
               offsets[chan] = cast_type(bld_base, offsets[chan], nir_type_int, 32);
            }
         }
         break;
      }
      case nir_tex_src_ms_index:
         break;
      default:
         assert(0);
         break;
      }
   }
   if (!sampler_deref_instr)
      sampler_deref_instr = texture_deref_instr;

   if (explicit_lod)
      lod_property = lp_build_nir_lod_property(bld_base, instr->src[lod_src].src);

   if (instr->op == nir_texop_tex || instr->op == nir_texop_tg4 || instr->op == nir_texop_txb ||
       instr->op == nir_texop_txl || instr->op == nir_texop_txd || instr->op == nir_texop_lod)
      for (unsigned chan = 0; chan < instr->coord_components; ++chan)
         coords[chan] = cast_type(bld_base, coords[chan], nir_type_float, 32);
   else if (instr->op == nir_texop_txf || instr->op == nir_texop_txf_ms)
      for (unsigned chan = 0; chan < instr->coord_components; ++chan)
         coords[chan] = cast_type(bld_base, coords[chan], nir_type_int, 32);

   if (instr->is_array && instr->sampler_dim == GLSL_SAMPLER_DIM_1D) {
      /* move layer coord for 1d arrays. */
      coords[2] = coords[1];
      coords[1] = coord_undef;
   }

   if (projector) {
      for (unsigned chan = 0; chan < instr->coord_components; ++chan)
         coords[chan] = lp_build_mul(&bld_base->base, coords[chan], projector);
      if (sample_key & LP_SAMPLER_SHADOW)
         coords[4] = lp_build_mul(&bld_base->base, coords[4], projector);
   }

   uint32_t base_index = 0;
   if (!texture_deref_instr) {
      int samp_src_index = nir_tex_instr_src_index(instr, nir_tex_src_sampler_handle);
      if (samp_src_index == -1) {
         base_index = instr->sampler_index;
      }
   }

   if (instr->op == nir_texop_txd) {
      sample_key |= LP_SAMPLER_LOD_DERIVATIVES << LP_SAMPLER_LOD_CONTROL_SHIFT;
      params.derivs = &derivs;
      if (bld_base->shader->info.stage == MESA_SHADER_FRAGMENT) {
         if (gallivm_perf & GALLIVM_PERF_NO_QUAD_LOD)
            lod_property = LP_SAMPLER_LOD_PER_ELEMENT;
         else
            lod_property = LP_SAMPLER_LOD_PER_QUAD;
      } else
         lod_property = LP_SAMPLER_LOD_PER_ELEMENT;
   }

   sample_key |= lod_property << LP_SAMPLER_LOD_PROPERTY_SHIFT;
   params.sample_key = sample_key;
   params.offsets = offsets;
   params.texture_index = base_index;
   params.sampler_index = base_index;
   params.coords = coords;
   params.texel = texel;
   params.lod = explicit_lod;
   bld_base->tex(bld_base, &params);
   assign_dest(bld_base, &instr->dest, texel);
}

static void visit_ssa_undef(struct lp_build_nir_context *bld_base,
                            const nir_ssa_undef_instr *instr)
{
   unsigned num_components = instr->def.num_components;
   LLVMValueRef undef[NIR_MAX_VEC_COMPONENTS];
   struct lp_build_context *undef_bld = get_int_bld(bld_base, true, instr->def.bit_size);
   for (unsigned i = 0; i < num_components; i++)
      undef[i] = LLVMGetUndef(undef_bld->vec_type);
   assign_ssa_dest(bld_base, &instr->def, undef);
}

static void visit_jump(struct lp_build_nir_context *bld_base,
                       const nir_jump_instr *instr)
{
   switch (instr->type) {
   case nir_jump_break:
      bld_base->break_stmt(bld_base);
      break;
   case nir_jump_continue:
      bld_base->continue_stmt(bld_base);
      break;
   default:
      unreachable("Unknown jump instr\n");
   }
}

static void visit_deref(struct lp_build_nir_context *bld_base,
                        nir_deref_instr *instr)
{
   if (instr->mode != nir_var_mem_shared &&
       instr->mode != nir_var_mem_global)
      return;
   LLVMValueRef result = NULL;
   switch(instr->deref_type) {
   case nir_deref_type_var: {
      struct hash_entry *entry = _mesa_hash_table_search(bld_base->vars, instr->var);
      result = entry->data;
      break;
   }
   default:
      unreachable("Unhandled deref_instr deref type");
   }

   assign_ssa(bld_base, instr->dest.ssa.index, result);
}

static void visit_block(struct lp_build_nir_context *bld_base, nir_block *block)
{
   nir_foreach_instr(instr, block)
   {
      switch (instr->type) {
      case nir_instr_type_alu:
         visit_alu(bld_base, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_load_const:
         visit_load_const(bld_base, nir_instr_as_load_const(instr));
         break;
      case nir_instr_type_intrinsic:
         visit_intrinsic(bld_base, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_tex:
         visit_tex(bld_base, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_phi:
         assert(0);
         break;
      case nir_instr_type_ssa_undef:
         visit_ssa_undef(bld_base, nir_instr_as_ssa_undef(instr));
         break;
      case nir_instr_type_jump:
         visit_jump(bld_base, nir_instr_as_jump(instr));
         break;
      case nir_instr_type_deref:
         visit_deref(bld_base, nir_instr_as_deref(instr));
         break;
      default:
         fprintf(stderr, "Unknown NIR instr type: ");
         nir_print_instr(instr, stderr);
         fprintf(stderr, "\n");
         abort();
      }
   }
}

static void visit_if(struct lp_build_nir_context *bld_base, nir_if *if_stmt)
{
   LLVMValueRef cond = get_src(bld_base, if_stmt->condition);

   bld_base->if_cond(bld_base, cond);
   visit_cf_list(bld_base, &if_stmt->then_list);

   if (!exec_list_is_empty(&if_stmt->else_list)) {
      bld_base->else_stmt(bld_base);
      visit_cf_list(bld_base, &if_stmt->else_list);
   }
   bld_base->endif_stmt(bld_base);
}

static void visit_loop(struct lp_build_nir_context *bld_base, nir_loop *loop)
{
   bld_base->bgnloop(bld_base);
   visit_cf_list(bld_base, &loop->body);
   bld_base->endloop(bld_base);
}

static void visit_cf_list(struct lp_build_nir_context *bld_base,
                          struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list)
   {
      switch (node->type) {
      case nir_cf_node_block:
         visit_block(bld_base, nir_cf_node_as_block(node));
         break;

      case nir_cf_node_if:
         visit_if(bld_base, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         visit_loop(bld_base, nir_cf_node_as_loop(node));
         break;

      default:
         assert(0);
      }
   }
}

static void
handle_shader_output_decl(struct lp_build_nir_context *bld_base,
                          struct nir_shader *nir,
                          struct nir_variable *variable)
{
   bld_base->emit_var_decl(bld_base, variable);
}

/* vector registers are stored as arrays in LLVM side,
   so we can use GEP on them, as to do exec mask stores
   we need to operate on a single components.
   arrays are:
   0.x, 1.x, 2.x, 3.x
   0.y, 1.y, 2.y, 3.y
   ....
*/
static LLVMTypeRef get_register_type(struct lp_build_nir_context *bld_base,
                                     nir_register *reg)
{
   struct lp_build_context *int_bld = get_int_bld(bld_base, true, reg->bit_size);

   LLVMTypeRef type = int_bld->vec_type;
   if (reg->num_array_elems)
      type = LLVMArrayType(type, reg->num_array_elems);
   if (reg->num_components > 1)
      type = LLVMArrayType(type, reg->num_components);

   return type;
}


bool lp_build_nir_llvm(
   struct lp_build_nir_context *bld_base,
   struct nir_shader *nir)
{
   struct nir_function *func;

   nir_convert_from_ssa(nir, true);
   nir_lower_locals_to_regs(nir);
   nir_remove_dead_derefs(nir);
   nir_remove_dead_variables(nir, nir_var_function_temp);

   nir_foreach_variable(variable, &nir->outputs)
      handle_shader_output_decl(bld_base, nir, variable);

   bld_base->regs = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                            _mesa_key_pointer_equal);
   bld_base->vars = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                            _mesa_key_pointer_equal);

   func = (struct nir_function *)exec_list_get_head(&nir->functions);

   nir_foreach_register(reg, &func->impl->registers) {
      LLVMTypeRef type = get_register_type(bld_base, reg);
      LLVMValueRef reg_alloc = lp_build_alloca_undef(bld_base->base.gallivm,
                                                     type, "reg");
      _mesa_hash_table_insert(bld_base->regs, reg, reg_alloc);
   }
   nir_index_ssa_defs(func->impl);
   bld_base->ssa_defs = calloc(func->impl->ssa_alloc, sizeof(LLVMValueRef));
   visit_cf_list(bld_base, &func->impl->body);

   free(bld_base->ssa_defs);
   ralloc_free(bld_base->vars);
   ralloc_free(bld_base->regs);
   return true;
}

/* do some basic opts to remove some things we don't want to see. */
void lp_build_opt_nir(struct nir_shader *nir)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS_V(nir, nir_opt_constant_folding);
      NIR_PASS_V(nir, nir_opt_algebraic);
      NIR_PASS_V(nir, nir_lower_pack);
   } while (progress);
   nir_lower_bool_to_int32(nir);
}