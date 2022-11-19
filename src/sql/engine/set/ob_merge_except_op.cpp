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

#include "ob_merge_except_op.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

ObMergeExceptSpec::ObMergeExceptSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
  : ObMergeSetSpec(alloc, type)
{}

OB_SERIALIZE_MEMBER((ObMergeExceptSpec, ObMergeSetSpec));

ObMergeExceptOp::ObMergeExceptOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
  : ObMergeSetOp(exec_ctx, spec, input),
  right_iter_end_(false),
  first_got_right_row_(true),
  first_got_left_row_(true),
  right_brs_(nullptr),
  right_idx_(0)
{}

int ObMergeExceptOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == left_ || nullptr == right_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected status: left or right is null", K(ret));
  } else if (OB_FAIL(ObMergeSetOp::inner_open())) {
    LOG_WARN("failed to init open", K(ret));
  } else {
    need_skip_init_row_ = true;
  }
  return ret;
}

int ObMergeExceptOp::inner_close()
{
  return ObMergeSetOp::inner_close();
}

int ObMergeExceptOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObMergeSetOp::inner_rescan())) {
    LOG_WARN("failed to rescan", K(ret));
  } else {
    right_iter_end_ = false;
    first_got_right_row_ = true;
    need_skip_init_row_ = true;
    first_got_left_row_ = true;
    right_brs_ = nullptr;
    right_idx_ = 0;
  }
  return ret;
}

void ObMergeExceptOp::destroy()
{
  ObMergeSetOp::destroy();
}

int ObMergeExceptOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  int cmp = 0;
  bool break_outer_loop = false;
  const ObIArray<ObExpr*> *left_row = NULL;
  clear_evaluated_flag();
  while (OB_SUCC(ret) && OB_SUCC(do_strict_distinct(*left_, last_row_.store_row_, left_row))) {
    break_outer_loop = right_iter_end_;
    while (OB_SUCC(ret) && !right_iter_end_) {
      if (!first_got_right_row_) {
        if (OB_FAIL(cmp_(right_->get_spec().output_, *left_row, eval_ctx_, cmp))) {
          LOG_WARN("cmp_ input_row with right_row failed", K(*left_row), K(ret));
        } else if (cmp > 0) {
          //input_row is not in the right set, output input_row
          break_outer_loop = true;
          break;
        } else if (0 == cmp) {
          break;
        } else {
          if (OB_FAIL(right_->get_next_row())) {
            if (OB_ITER_END == ret) {
              right_iter_end_ = true;
              break_outer_loop = true;
              ret = OB_SUCCESS;
            } else {
              LOG_WARN("fail to get right operator row", K(ret));
            }
          } else {}
        }
      } else {
        //get first row
        first_got_right_row_ = false;
        if (OB_FAIL(right_->get_next_row())) {
          if (OB_ITER_END == ret) {
            right_iter_end_ = true;
            break_outer_loop = true;
            ret = OB_SUCCESS;
          } else {
            LOG_WARN("fail to get right operator row", K(ret));
          }
        }
      }
    }
    if (break_outer_loop) {
      break; //cmp < 0 implement that the left_row is not in the right set, so output it
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(convert_row(*left_row, MY_SPEC.set_exprs_))) {
      LOG_WARN("failed to convert row", K(ret));
    } else if (OB_FAIL(last_row_.save_store_row(*left_row, eval_ctx_, 0))) {
      LOG_WARN("failed to save right row", K(ret));
    }
  }
  return ret;
}

int ObMergeExceptOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = std::min(max_row_cnt, MY_SPEC.max_batch_size_);
  // first, we got a batch from left
  // then for every strict distinct row in left, we locate next row from right
  // when we end the batch, store last valid left row
  clear_evaluated_flag();
  const ObBatchRows *left_brs = nullptr;
  int64_t curr_left_idx = 0;
  int64_t last_left_idx = 0;
  if (OB_FAIL(left_->get_next_batch(batch_size, left_brs))) {
    LOG_WARN("failed to get next batch", K(ret));
  } else if (left_brs->end_ && 0 == left_brs->size_) {
    ret = OB_ITER_END;
  } else {
    int cmp = 0;
    //set all rows skiped here, when we got a valid row, reset to 0
    brs_.skip_->set_all(left_brs->size_);
    brs_.size_ = left_brs->size_;
    while (OB_SUCC(ret) && OB_SUCC(locate_next_left_inside(*left_,
                                                           last_left_idx,
                                                           *left_brs,
                                                           curr_left_idx,
                                                           first_got_left_row_))) {
      //when we succ locate a row in left batch, store row is out of date,
      //we will compare inside a batch
      last_row_.store_row_ = nullptr;
      brs_.skip_->unset(curr_left_idx);
      if (right_iter_end_) {
        last_left_idx = curr_left_idx;
        ++curr_left_idx;
      }
      while (OB_SUCC(ret) && !right_iter_end_) {
        if (!first_got_right_row_) {
          if (OB_FAIL(cmp_(right_->get_spec().output_, left_->get_spec().output_,
                           right_idx_, curr_left_idx, eval_ctx_, cmp))) {
            LOG_WARN("failed to compare rows", K(ret));
          } else if (0 == cmp) {
            brs_.skip_->set(curr_left_idx);
            last_left_idx = curr_left_idx;
            ++curr_left_idx;
            ++right_idx_;
            if (OB_FAIL(locate_next_right(*right_, batch_size, right_brs_, right_idx_))) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
                right_iter_end_ = true;
              } else {
                LOG_WARN("failed to locate next right", K(ret));
              }
            }
            break;
          } else if (cmp > 0) {
            //move left idx
            last_left_idx = curr_left_idx;
            ++curr_left_idx;
            break;
          } else {
            ++right_idx_;
            if (OB_FAIL(locate_next_right(*right_, batch_size, right_brs_, right_idx_))) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
                right_iter_end_ = true;
              } else {
                LOG_WARN("failed to locate next right", K(ret));
              }
            }
          }

        } else {
          first_got_right_row_ = false;
          if (OB_FAIL(locate_next_right(*right_, batch_size, right_brs_, right_idx_))) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              right_iter_end_ = true;
            } else {
              LOG_WARN("failed to locate next right", K(ret));
            }
          }
        }
      }
    }
  }
  if (OB_SUCCESS == ret) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("unexpected status", K(ret));
  } else if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
    if (OB_FAIL(convert_batch(left_->get_spec().output_, MY_SPEC.set_exprs_, brs_))) {
      LOG_WARN("failed to convert batch", K(ret));
    } else if (left_brs->end_) {
      brs_.end_ = true;
    } else if (last_left_idx == left_brs->size_) {
      // empty batch
    } else {
      ObEvalCtx::BatchInfoScopeGuard batch_info_guard(eval_ctx_);
      batch_info_guard.set_batch_idx(last_left_idx);
      if (OB_FAIL(last_row_.save_store_row(left_->get_spec().output_, eval_ctx_, 0))) {
        LOG_WARN("failed to save last row", K(ret));
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
