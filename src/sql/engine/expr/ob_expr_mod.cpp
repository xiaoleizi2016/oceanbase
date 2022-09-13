/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG

#include <math.h>
#include "sql/engine/expr/ob_expr_mod.h"
//#include "sql/engine/expr/ob_expr_promotion_util.h"
#include "sql/engine/expr/ob_expr_result_type_util.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase {
using namespace common;
namespace sql {

constexpr double EPSILON = 0.000000001;

ObExprMod::ObExprMod(ObIAllocator& alloc)
    : ObArithExprOperator(alloc, T_OP_MOD, N_MOD, 2, NOT_ROW_DIMENSION, ObExprResultTypeUtil::get_mod_result_type,
          ObExprResultTypeUtil::get_mod_calc_type, mod_funcs_)
{
  param_lazy_eval_ = true;
}

int ObExprMod::calc_result_type2(
    ObExprResType& type, ObExprResType& type1, ObExprResType& type2, ObExprTypeCtx& type_ctx) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObArithExprOperator::calc_result_type2(type, type1, type2, type_ctx))) {
  } else if (lib::is_oracle_mode() && type.is_oracle_decimal()) {
    type.set_scale(ORA_NUMBER_SCALE_UNKNOWN_YET);
    type.set_precision(PRECISION_UNKNOWN_YET);
  } else {
    ObScale scale1 = static_cast<ObScale>(MAX(type1.get_scale(), 0));
    ObScale scale2 = static_cast<ObScale>(MAX(type2.get_scale(), 0));
    if (OB_UNLIKELY(SCALE_UNKNOWN_YET == type1.get_scale()) || OB_UNLIKELY(SCALE_UNKNOWN_YET == type2.get_scale())) {
      type.set_scale(SCALE_UNKNOWN_YET);
    } else {
      type.set_scale(MAX(scale1, scale2));
      type.set_precision(MAX(type1.get_precision(), type2.get_precision()));
    }
  }
  return ret;
}

int ObExprMod::calc_result2(ObObj& res, const ObObj& left, const ObObj& right, ObExprCtx& expr_ctx) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(param_eval(expr_ctx, left, 0))) {
    LOG_WARN("failed to calculate parameter 0", K(ret));
  } else if (OB_UNLIKELY(ObNullType == left.get_type())) {
    res.set_null();
  } else if (OB_FAIL(param_eval(expr_ctx, right, 1))) {
    LOG_WARN("failed to calculate parameter 1", K(ret));
  } else if (OB_UNLIKELY(ObNullType == right.get_type())) {
    res.set_null();
  } else {
    ObObjTypeClass tc1 = left.get_type_class();
    ObObjTypeClass tc2 = right.get_type_class();
    ObScale calc_scale = result_type_.get_calc_scale();
    ObObjType calc_type = result_type_.get_calc_type();
    if (ObIntTC == tc1) {
      if (ObIntTC == tc2 || ObUIntTC == tc2) {
        ret = mod_int(res, left, right, expr_ctx.calc_buf_, calc_scale);
      } else {
        ret = ObArithExprOperator::calc_(res, left, right, expr_ctx, calc_scale, calc_type, mod_funcs_);
      }
    } else if (ObUIntTC == tc1) {
      if (ObIntTC == tc2 || ObUIntTC == tc2) {
        ret = mod_uint(res, left, right, expr_ctx.calc_buf_, calc_scale);
      } else {
        ret = ObArithExprOperator::calc_(res, left, right, expr_ctx, calc_scale, calc_type, mod_funcs_);
      }
    } else {
      ret = ObArithExprOperator::calc_(res, left, right, expr_ctx, calc_scale, calc_type, mod_funcs_);
    }
  }
  if (OB_SUCC(ret) && result_type_.get_type() != res.get_type() && res.get_type() != ObNullType) {
    const ObObj* res_obj = NULL;
    EXPR_DEFINE_CAST_CTX(expr_ctx, CM_NONE);
    EXPR_CAST_OBJ_V2(result_type_.get_type(), res, res_obj);
    if (OB_FAIL(ret)) {
      LOG_WARN("cast failed", K(ret), K(left), K(right));
    } else if (OB_ISNULL(res_obj)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null pointer", K(ret), K(left), K(right));
    } else {
      res = *res_obj;
    }
  }
  return ret;
}

int ObExprMod::calc(ObObj& res, const ObObj& left, const ObObj& right, ObIAllocator* allocator, ObScale scale)
{
  ObCalcTypeFunc calc_type_func = ObExprResultTypeUtil::get_mod_result_type;
  return ObArithExprOperator::calc(res, left, right, allocator, scale, calc_type_func, mod_funcs_);
}

ObArithFunc ObExprMod::mod_funcs_[ObMaxTC] = {
    NULL,
    ObExprMod::mod_int,
    ObExprMod::mod_uint,
    ObExprMod::mod_float,
    ObExprMod::mod_double,
    ObExprMod::mod_number,
    NULL,  // datetime
    NULL,  // date
    NULL,  // time
    NULL,  // year
    NULL,  // varchar
    NULL,  // extend
    NULL,  // unknown
    NULL,  // text
    NULL,  // bit
    NULL,  // enumset
    NULL,  // enumsetInner
};

int ObExprMod::mod_int(ObObj& res, const ObObj& left, const ObObj& right, ObIAllocator* allocator, ObScale scale)
{
  int ret = OB_SUCCESS;
  int64_t left_i = left.get_int();
  int64_t right_i = right.get_int();
  if (OB_UNLIKELY(0 == right_i)) {
    if (lib::is_oracle_mode()) {
      res.set_int(left_i);
    } else {
      res.set_null();
    }
  } else if (OB_LIKELY(left.get_type_class() == right.get_type_class())) {
    if (INT64_MIN == left_i && -1 == right_i) {
      res.set_int(0);  // INT64_MIN % -1 --> FPE
    } else {
      res.set_int(left_i % right_i);
    }
  } else if (OB_UNLIKELY(ObUIntTC != right.get_type_class())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid types", K(ret), K(left), K(right));
  } else {
    if (left_i < 0) {
      res.set_int(-static_cast<int64_t>(-left_i % static_cast<uint64_t>(right_i)));
    } else {
      res.set_int(static_cast<int64_t>(left_i % static_cast<uint64_t>(right_i)));
    }
  }
  UNUSED(allocator);
  UNUSED(scale);
  return ret;
}

int ObExprMod::mod_uint(ObObj& res, const ObObj& left, const ObObj& right, ObIAllocator* allocator, ObScale scale)
{
  int ret = OB_SUCCESS;
  uint64_t left_ui = left.get_uint64();
  uint64_t right_ui = right.get_uint64();
  if (OB_UNLIKELY(0 == right_ui)) {
    if (lib::is_oracle_mode()) {
      res.set_uint64(left_ui);
    } else {
      res.set_null();
    }
  } else if (OB_LIKELY(left.get_type_class() == right.get_type_class())) {
    res.set_uint64(left_ui % right_ui);
  } else if (OB_UNLIKELY(ObIntTC != right.get_type_class())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid types", K(ret), K(left), K(right));
  } else {
    if (static_cast<int64_t>(right_ui) < 0) {
      res.set_uint64(left_ui % -static_cast<int64_t>(right_ui));
    } else {
      res.set_uint64(left_ui % right_ui);
    }
  }
  UNUSED(allocator);
  UNUSED(scale);
  return ret;
}

int ObExprMod::mod_float(ObObj& res, const ObObj& left, const ObObj& right, ObIAllocator* allocator, ObScale scale)
{

  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!lib::is_oracle_mode())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("only oracle mode arrive here", K(ret), K(left), K(right));
  } else if (OB_UNLIKELY(left.get_type_class() != right.get_type_class())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid types", K(ret), K(left), K(right));
  } else if (fabsf(right.get_float()) < EPSILON) {
    res.set_float(left.get_float());
  } else {
    res.set_float(fmodf(left.get_float(), right.get_float()));
    LOG_DEBUG("succ to mod float", K(res), K(left), K(right));
  }
  UNUSED(allocator);
  UNUSED(scale);
  return ret;
}

int ObExprMod::mod_double(ObObj& res, const ObObj& left, const ObObj& right, ObIAllocator* allocator, ObScale scale)
{

  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(left.get_type_class() != right.get_type_class())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid types", K(ret), K(left), K(right));
  } else if (fabs(right.get_double()) < EPSILON) {
    if (lib::is_oracle_mode()) {
      res.set_double(left.get_double());
    } else {
      res.set_null();
    }
  } else {
    res.set_double(fmod(left.get_double(), right.get_double()));
    LOG_DEBUG("succ to mod double", K(res), K(left), K(right));
  }
  UNUSED(allocator);
  UNUSED(scale);
  return ret;
}

int ObExprMod::mod_number(ObObj& res, const ObObj& left, const ObObj& right, ObIAllocator* allocator, ObScale scale)
{
  int ret = OB_SUCCESS;
  number::ObNumber res_nmb;
  if (OB_UNLIKELY(NULL == allocator)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_ERROR("allocator is null", K(ret));
  } else if (OB_UNLIKELY(right.is_zero())) {
    if (lib::is_oracle_mode()) {
      res.set_number(left.get_number());
    } else {
      res.set_null();
    }
  } else if (OB_FAIL(left.get_number().rem_v3(right.get_number(), res_nmb, *allocator))) {
    LOG_WARN("failed to rem numbers", K(ret), K(left), K(right));
  } else {
    res.set_number(res_nmb);
  }
  UNUSED(scale);
  return ret;
}

int ObExprMod::mod_int_int(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    int64_t left_i = left->get_int();
    int64_t right_i = right->get_int();
    if (OB_UNLIKELY(0 == right_i)) {
      if (lib::is_oracle_mode()) {
        datum.set_int(left_i);
      } else {
        datum.set_null();
      }
    } else {
      if (INT64_MIN == left_i && -1 == right_i) {
        datum.set_int(0);  // INT64_MIN % -1 --> FPE
      } else {
        datum.set_int(left_i % right_i);
      }
    }
  }
  return ret;
}

int ObExprMod::mod_int_uint(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    int64_t left_i = left->get_int();
    uint64_t right_ui = right->get_uint();
    if (OB_UNLIKELY(0 == right_ui)) {
      if (lib::is_oracle_mode()) {
        datum.set_int(left_i);
      } else {
        datum.set_null();
      }
    } else {
      if (left_i < 0) {
        datum.set_int(-static_cast<int64_t>(-left_i % right_ui));
      } else {
        datum.set_int(static_cast<int64_t>(left_i % right_ui));
      }
    }
  }
  return ret;
}

int ObExprMod::mod_uint_int(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    uint64_t left_ui = left->get_uint();
    int64_t right_i = right->get_int();
    if (OB_UNLIKELY(0 == right_i)) {
      if (lib::is_oracle_mode()) {
        datum.set_uint(left_ui);
      } else {
        datum.set_null();
      }
    } else {
      datum.set_uint(left_ui % labs(right_i));
    }
  }
  return ret;
}

int ObExprMod::mod_uint_uint(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    uint64_t left_ui = left->get_uint();
    uint64_t right_ui = right->get_uint();
    if (OB_UNLIKELY(0 == right_ui)) {
      if (lib::is_oracle_mode()) {
        datum.set_uint(left_ui);
      } else {
        datum.set_null();
      }
    } else {
      datum.set_uint(left_ui % right_ui);
    }
  }
  return ret;
}

int ObExprMod::mod_float(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_UNLIKELY(!lib::is_oracle_mode())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("only oracle mode arrive here", K(ret), K(left), K(right));
  } else if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    const float left_f = left->get_float();
    const float right_f = right->get_float();
    if (fabsf(right_f) < share::ObUnitConfig::CPU_EPSILON) {
      datum.set_float(left_f);
    } else {
      datum.set_float(fmodf(left_f, right_f));
      LOG_DEBUG("succ to mod float", K(datum), K(left_f), K(right_f));
    }
  }
  return ret;
}

int ObExprMod::mod_double(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    const double left_d = left->get_double();
    const double right_d = right->get_double();
    if (fabs(right_d) < EPSILON) {
      if (lib::is_oracle_mode()) {
        datum.set_double(left_d);
      } else {
        datum.set_null();
      }
    } else {
      datum.set_double(fmod(left_d, right_d));
      LOG_DEBUG("succ to mod double", K(datum), K(left_d), K(right_d));
    }
  }
  return ret;
}

int ObExprMod::mod_number(const ObExpr& expr, ObEvalCtx& ctx, ObDatum& datum)
{
  int ret = OB_SUCCESS;
  ObDatum* left = NULL;
  ObDatum* right = NULL;
  bool is_finish = false;
  if (OB_FAIL(get_arith_operand(expr, ctx, left, right, datum, is_finish))) {
    LOG_WARN("get_arith_operand failed", K(ret));
  } else if (is_finish) {
    // do nothing
  } else {
    const number::ObNumber lnum(left->get_number());
    const number::ObNumber rnum(right->get_number());
    if (OB_UNLIKELY(rnum.is_zero())) {
      if (lib::is_oracle_mode()) {
        datum.set_number(lnum);
      } else {
        datum.set_null();
      }
    } else {
      char local_buff[number::ObNumber::MAX_BYTE_LEN];
      ObDataBuffer local_alloc(local_buff, number::ObNumber::MAX_BYTE_LEN);
      number::ObNumber result;
      if (OB_FAIL(lnum.rem_v3(rnum, result, local_alloc))) {
        LOG_WARN("failed to rem numbers", K(lnum), K(rnum), K(ret));
      } else {
        datum.set_number(result);
      }
    }
  }
  return ret;
}

int ObExprMod::cg_expr(ObExprCGCtx& op_cg_ctx, const ObRawExpr& raw_expr, ObExpr& rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(raw_expr);
  UNUSED(op_cg_ctx);
  OB_ASSERT(2 == rt_expr.arg_cnt_);
  OB_ASSERT(NULL != rt_expr.args_);
  OB_ASSERT(NULL != rt_expr.args_[0]);
  OB_ASSERT(NULL != rt_expr.args_[1]);
  const common::ObObjType left = rt_expr.args_[0]->datum_meta_.type_;
  const common::ObObjType right = rt_expr.args_[1]->datum_meta_.type_;
  const ObObjTypeClass right_tc = ob_obj_type_class(right);
  OB_ASSERT(left == input_types_[0].get_calc_type());
  OB_ASSERT(right == input_types_[1].get_calc_type());

  rt_expr.inner_functions_ = NULL;
  LOG_DEBUG("arrive here cg_expr", K(ret), K(rt_expr));
  switch (rt_expr.datum_meta_.type_) {
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType: {
      if (right_tc == ObIntTC) {
        rt_expr.eval_func_ = ObExprMod::mod_int_int;
      } else if (right_tc == ObUIntTC) {
        rt_expr.eval_func_ = ObExprMod::mod_int_uint;
      }
      break;
    }
    case ObUTinyIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type: {
      if (right_tc == ObIntTC) {
        rt_expr.eval_func_ = ObExprMod::mod_uint_int;
      } else if (right_tc == ObUIntTC) {
        rt_expr.eval_func_ = ObExprMod::mod_uint_uint;
      }
      break;
    }
    case ObFloatType: {
      rt_expr.eval_func_ = ObExprMod::mod_float;
      break;
    }
    case ObUDoubleType:
    case ObDoubleType: {
      rt_expr.eval_func_ = ObExprMod::mod_double;
      break;
    }
    case ObUNumberType:
    case ObNumberType: {
      rt_expr.eval_func_ = ObExprMod::mod_number;
      break;
    }
    default: {
      // do nothing
      break;
    }
  }

  if (OB_ISNULL(rt_expr.eval_func_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected result type", K(ret), K(rt_expr.datum_meta_.type_), K(left), K(right));
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
