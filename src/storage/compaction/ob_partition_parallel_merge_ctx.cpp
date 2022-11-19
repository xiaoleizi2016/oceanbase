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

#include "ob_partition_parallel_merge_ctx.h"
#include "storage/memtable/ob_memtable.h"
#include "share/config/ob_server_config.h"
#include "observer/omt/ob_tenant_config_mgr.h"
#include "storage/ob_partition_range_spliter.h"
#include "ob_tablet_merge_ctx.h"
#include "share/scheduler/ob_dag_scheduler.h"
#include "storage/blocksstable/ob_sstable.h"
namespace oceanbase
{
using namespace common;
using namespace share::schema;
using namespace share;
using namespace blocksstable;

namespace storage
{

ObParallelMergeCtx::ObParallelMergeCtx()
  : parallel_type_(INVALID_PARALLEL_TYPE),
    range_array_(),
    concurrent_cnt_(0),
    allocator_("paralMergeCtx", OB_MALLOC_NORMAL_BLOCK_SIZE),
    is_inited_(false)
{
}

ObParallelMergeCtx::~ObParallelMergeCtx()
{
  reset();
}

void ObParallelMergeCtx::reset()
{
  parallel_type_ = INVALID_PARALLEL_TYPE;
  range_array_.reset();
  concurrent_cnt_ = 0;
  allocator_.reset();
  is_inited_ = false;
}

bool ObParallelMergeCtx::is_valid() const
{
  bool bret = true;
  if (IS_NOT_INIT || concurrent_cnt_ <= 0 || parallel_type_ >= INVALID_PARALLEL_TYPE) {
    bret = false;
  } else if (range_array_.count() != concurrent_cnt_) {
    bret = false;
  } else if (concurrent_cnt_ > 1 && SERIALIZE_MERGE == parallel_type_) {
    bret = false;
  }
  return bret;
}

int ObParallelMergeCtx::init(compaction::ObTabletMergeCtx &merge_ctx)
{
  int ret = OB_SUCCESS;

  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObParallelMergeCtx init twice", K(ret));
  } else if (OB_UNLIKELY(!merge_ctx.is_schema_valid() || merge_ctx.tables_handle_.empty())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel merge", K(ret), K(merge_ctx));
  } else {
    int64_t tablet_size = merge_ctx.get_merge_schema()->get_tablet_size();
    bool enable_parallel_minor_merge = false;
    {
      omt::ObTenantConfigGuard tenant_config(TENANT_CONF(MTL_ID()));
      if (tenant_config.is_valid()) {
        enable_parallel_minor_merge = tenant_config->_enable_parallel_minor_merge;
      }
    } // end of ObTenantConfigGuard
    if (enable_parallel_minor_merge && tablet_size > 0 && merge_ctx.param_.is_mini_merge()) {
      if (OB_FAIL(init_parallel_mini_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to init parallel setting for mini merge", K(ret));
      }
    } else if (enable_parallel_minor_merge && tablet_size > 0 && merge_ctx.param_.is_minor_merge()) {
      if (OB_FAIL(init_parallel_mini_minor_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to init parallel setting for mini minor merge", K(ret));
      }
    } else if (tablet_size > 0 && merge_ctx.param_.is_major_merge()) {
      if (OB_FAIL(init_parallel_major_merge(merge_ctx))) {
        STORAGE_LOG(WARN, "Failed to init parallel major merge", K(ret));
      }
    } else if (OB_FAIL(init_serial_merge())) {
      STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
    }
    if (OB_SUCC(ret)) {
      is_inited_ = true;
      STORAGE_LOG(INFO, "Succ to init parallel merge ctx",
          K(enable_parallel_minor_merge), K(tablet_size), K(merge_ctx.param_));
    }
  }

  return ret;
}


int ObParallelMergeCtx::get_merge_range(const int64_t parallel_idx, ObDatumRange &merge_range)
{
  int ret = OB_SUCCESS;

  if (!is_valid()) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObParallelMergeCtx is not inited", K(ret), K(*this));
  } else if (parallel_idx >= concurrent_cnt_) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to get parallel mergerange", K(ret), K(parallel_idx),
                K_(concurrent_cnt));
  } else {
    switch (parallel_type_) {
      case PARALLEL_MAJOR:
      case PARALLEL_MINI:
      case PARALLEL_MINI_MINOR:
      case SERIALIZE_MERGE:
        merge_range = range_array_.at(parallel_idx);
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(ERROR, "Unexpected parallel merge type", K(ret), K(*this));
        break;
    }
  }

  return ret;
}

int ObParallelMergeCtx::init_serial_merge()
{
  int ret = OB_SUCCESS;
  ObDatumRange merge_range;
  merge_range.set_whole_range();
  range_array_.reset();
  if (OB_FAIL(range_array_.push_back(merge_range))) {
    STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(merge_range));
  } else {
    concurrent_cnt_ = 1;
    parallel_type_ = SERIALIZE_MERGE;
  }

  return ret;
}

int ObParallelMergeCtx::init_parallel_major_merge(compaction::ObTabletMergeCtx &merge_ctx)
{
  int ret = OB_SUCCESS;
  const ObITable *first_table = nullptr;
  if (OB_UNLIKELY(MAJOR_MERGE != merge_ctx.param_.merge_type_)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel major merge", K(ret), K(merge_ctx));
  } else if (OB_UNLIKELY(nullptr == (first_table = merge_ctx.tables_handle_.get_table(0))
      || !first_table->is_sstable())) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(WARN, "Unexpected first table", K(ret), K(merge_ctx.tables_handle_));
  } else {
    const int64_t tablet_size = merge_ctx.schema_ctx_.merge_schema_->get_tablet_size();
    const ObSSTable *first_sstable = static_cast<const ObSSTable *>(first_table);
    const int64_t macro_block_cnt = first_sstable->get_meta().get_macro_info().get_data_block_ids().count();
    if (OB_FAIL(get_concurrent_cnt(tablet_size, macro_block_cnt, concurrent_cnt_))) {
      STORAGE_LOG(WARN, "failed to get concurrent cnt", K(ret), K(tablet_size), K(concurrent_cnt_),
        KPC(first_sstable));
    } else if (1 == concurrent_cnt_) {
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "failed to init serial merge", K(ret), KPC(first_sstable));
      }
    } else if (OB_FAIL(get_major_parallel_ranges(
        first_sstable, tablet_size, merge_ctx.tablet_handle_.get_obj()->get_index_read_info()))) {
      STORAGE_LOG(WARN, "Failed to get concurrent cnt from first sstable",
          K(ret), K(tablet_size), K_(concurrent_cnt));
    } else {
      parallel_type_ = PARALLEL_MAJOR;
    }
  }
  /*
  else {
    bool is_incremental = false;
    ObPartitionIncrementalRangeSpliter incremental_range_spliter;
    if (OB_FAIL(incremental_range_spliter.init(merge_ctx, allocator_))) {
      STORAGE_LOG(WARN, "Failed to init incremental range spliter", KR(ret), K(merge_ctx));
    } else if (OB_FAIL(incremental_range_spliter.check_is_incremental(is_incremental))) {
      STORAGE_LOG(WARN, "Failed to check is incremental", KR(ret));
    } else if (is_incremental) {
      // split ranges by incremental data
      if (OB_FAIL(incremental_range_spliter.split_ranges(range_array_))) {
        STORAGE_LOG(WARN, "Failed to split ranges", KR(ret));
      }
    } else {
      // split ranges by major sstable
      ObSEArray<ObStoreRange, 64> store_ranges;
      ObPartitionMajorSSTableRangeSpliter major_sstable_range_spliter;
      if (OB_ISNULL(merge_ctx.tables_handle_.get_table(0))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "unexpected null first table", K(ret), K(merge_ctx.tables_handle_));
      } else if (OB_UNLIKELY(!merge_ctx.tables_handle_.get_table(0)->is_major_sstable())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "first table must be major sstable", K(ret), K(merge_ctx.tables_handle_));
      } else if (OB_FAIL(major_sstable_range_spliter.init(
                   merge_ctx.tablet_handle_.get_obj()->get_index_read_info(),
                   static_cast<ObSSTable *>(merge_ctx.tables_handle_.get_table(0)),
                   merge_ctx.schema_ctx_.merge_schema_->get_tablet_size(),
                   allocator_))) {
        STORAGE_LOG(WARN, "Failed to init major sstable range spliter", KR(ret), K(merge_ctx));
      } else if (OB_FAIL(major_sstable_range_spliter.split_ranges(store_ranges))) {
        STORAGE_LOG(WARN, "Failed to split ranges", KR(ret));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < store_ranges.count(); i++) {
        ObDatumRange datum_range;
        if (OB_FAIL(datum_range.from_range(store_ranges.at(i), allocator_))) {
          STORAGE_LOG(WARN, "Failed to transfer store range to datum range", K(ret), K(i), K(store_ranges.at(i)));
        } else if (OB_FAIL(range_array_.push_back(datum_range))) {
          STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(datum_range));
        }
      }
    }
    if (OB_SUCC(ret)) {
      concurrent_cnt_ = range_array_.count();
      parallel_type_ = PARALLEL_MAJOR;
      STORAGE_LOG(INFO, "Succ to get parallel major merge ranges", K_(concurrent_cnt), K_(range_array));
    }
  }
  */
  return ret;
}

int ObParallelMergeCtx::init_parallel_mini_merge(compaction::ObTabletMergeCtx &merge_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(MINI_MERGE != merge_ctx.param_.merge_type_)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel mini merge", K(ret), K(merge_ctx));
  } else {
    const int64_t tablet_size = merge_ctx.get_merge_schema()->get_tablet_size();
    memtable::ObIMemtable *memtable = nullptr;
    if (OB_FAIL(merge_ctx.tables_handle_.get_first_memtable(memtable))) {
      STORAGE_LOG(WARN, "failed to get first memtable", K(ret),
                  "merge tables", merge_ctx.tables_handle_);
    } else {
      int64_t total_bytes = 0;
      int64_t total_rows = 0;
      int32_t mini_merge_thread = 0;
      if (OB_FAIL(memtable->estimate_phy_size(nullptr, nullptr, total_bytes, total_rows))) {
        STORAGE_LOG(WARN, "Failed to get estimate size from memtable", K(ret));
      } else if (MTL(ObTenantDagScheduler *)->get_up_limit(ObDagPrio::DAG_PRIO_COMPACTION_HIGH, mini_merge_thread)) {
        STORAGE_LOG(WARN, "failed to get uplimit", K(ret), K(mini_merge_thread));
      } else {
        ObArray<ObStoreRange> store_ranges;
        mini_merge_thread = MAX(mini_merge_thread, PARALLEL_MERGE_TARGET_TASK_CNT);
        concurrent_cnt_ = MIN((total_bytes + tablet_size - 1) / tablet_size, mini_merge_thread);
        if (concurrent_cnt_ <= 1) {
          if (OB_FAIL(init_serial_merge())) {
            STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
          }
        } else if (OB_FAIL(memtable->get_split_ranges(nullptr, nullptr, concurrent_cnt_, store_ranges))) {
          if (OB_ENTRY_NOT_EXIST == ret) {
            if (OB_FAIL(init_serial_merge())) {
              STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
            }
          } else {
            STORAGE_LOG(WARN, "Failed to get split ranges from memtable", K(ret));
          }
        } else if (OB_UNLIKELY(store_ranges.count() != concurrent_cnt_)) {
          ret = OB_ERR_UNEXPECTED;
          STORAGE_LOG(WARN, "Unexpected range array and concurrent_cnt", K(ret), K_(concurrent_cnt),
                      K(store_ranges));
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < store_ranges.count(); i++) {
            ObDatumRange datum_range;
            if (OB_FAIL(datum_range.from_range(store_ranges.at(i), allocator_))) {
              STORAGE_LOG(WARN, "Failed to transfer store range to datum range", K(ret), K(i), K(store_ranges.at(i)));
            } else if (OB_FAIL(range_array_.push_back(datum_range))) {
              STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(datum_range));
            }
          }
          parallel_type_ = PARALLEL_MINI;
          STORAGE_LOG(INFO, "Succ to get parallel mini merge ranges", K_(concurrent_cnt), K_(range_array));
        }
      }
    }
  }

  return ret;
}

int ObParallelMergeCtx::init_parallel_mini_minor_merge(compaction::ObTabletMergeCtx &merge_ctx)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!merge_ctx.param_.is_minor_merge())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to init parallel mini minor merge", K(ret), K(merge_ctx));
  } else {
    const ObTableReadInfo &index_read_info = merge_ctx.tablet_handle_.get_obj()->get_index_read_info();
    const int64_t tablet_size = merge_ctx.get_merge_schema()->get_tablet_size();
    ObRangeSplitInfo range_info;
    ObSEArray<ObITable *, DEFAULT_STORE_CNT_IN_STORAGE> tables;
    ObSEArray<ObStoreRange, 16> store_ranges;
    ObPartitionRangeSpliter range_spliter;
    ObStoreRange whole_range;
    whole_range.set_whole_range();
    if (OB_FAIL(merge_ctx.tables_handle_.get_all_minor_sstables(tables))) {
      STORAGE_LOG(WARN, "Failed to get all sstables from merge ctx", K(ret), K(merge_ctx));
    } else if (tables.count() != merge_ctx.tables_handle_.get_count()) {
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
      } else if (merge_ctx.param_.merge_type_ == MINI_MINOR_MERGE) {
        STORAGE_LOG(WARN, "Unexpected tables handle for mini minor merge", K(ret),
                  K(merge_ctx.tables_handle_));
      }
    } else if (OB_FAIL(range_spliter.get_range_split_info(tables, index_read_info, whole_range, range_info))) {
      STORAGE_LOG(WARN, "Failed to init range spliter", K(ret));
    } else if (OB_FAIL(calc_mini_minor_parallel_degree(tablet_size, range_info.total_size_, tables.count(),
                                                       range_info.parallel_target_count_))) {
      STORAGE_LOG(WARN, "Failed to calc mini minor parallel degree", K(ret));
    } else if (range_info.parallel_target_count_ <= 1) {
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
      }
    } else if (OB_FAIL(range_spliter.split_ranges(range_info, allocator_, true, store_ranges))) {
      STORAGE_LOG(WARN, "Failed to split parallel ranges", K(ret));
    } else if (OB_UNLIKELY(store_ranges.count() <= 1)) {
      range_spliter.reset();
      reset();
      if (OB_FAIL(init_serial_merge())) {
        STORAGE_LOG(WARN, "Failed to init serialize merge", K(ret));
      } else {
        STORAGE_LOG(INFO, "parallel minor merge back to serialize merge");
      }
    } else {
      concurrent_cnt_ = store_ranges.count();
      parallel_type_ = PARALLEL_MINI_MINOR;
      for (int64_t i = 0; OB_SUCC(ret) && i < store_ranges.count(); i++) {
        ObDatumRange datum_range;
        if (OB_FAIL(datum_range.from_range(store_ranges.at(i), allocator_))) {
          STORAGE_LOG(WARN, "Failed to transfer store range to datum range", K(ret), K(i), K(store_ranges.at(i)));
        } else if (OB_FAIL(range_array_.push_back(datum_range))) {
          STORAGE_LOG(WARN, "Failed to push back merge range to array", K(ret), K(datum_range));
        }
      }
      STORAGE_LOG(INFO, "Succ to get parallel mini minor merge ranges", K_(concurrent_cnt), K_(range_array));
    }
  }
  return ret;
}

int ObParallelMergeCtx::calc_mini_minor_parallel_degree(const int64_t tablet_size,
                                                        const int64_t total_size,
                                                        const int64_t sstable_count,
                                                        int64_t &parallel_degree)
{
  int ret = OB_SUCCESS;
  int32_t minor_merge_thread = 0;
  if (OB_UNLIKELY(tablet_size == 0 || total_size < 0 || sstable_count <= 1)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to calc mini minor parallel degree", K(ret), K(tablet_size),
                K(total_size), K(sstable_count));
  } else if (MTL(ObTenantDagScheduler *)->get_up_limit(ObDagPrio::DAG_PRIO_COMPACTION_MID, minor_merge_thread)) {
    STORAGE_LOG(WARN, "failed to get uplimit", K(ret), K(minor_merge_thread));
  } else {
    int64_t avg_sstable_size = total_size / sstable_count;
    parallel_degree = MIN(MAX(minor_merge_thread, PARALLEL_MERGE_TARGET_TASK_CNT),
                          (avg_sstable_size + tablet_size - 1) / tablet_size);
  }

  return ret;
}


int ObParallelMergeCtx::get_concurrent_cnt(
    const int64_t tablet_size,
    const int64_t macro_block_cnt,
    int64_t &concurrent_cnt)
{
  int ret = OB_SUCCESS;
  const int64_t max_merge_thread = MAX_MERGE_THREAD;
  if (OB_UNLIKELY(tablet_size < 0)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "tablet size is invalid", K(tablet_size), K(ret));
  } else if (0 == tablet_size) {
    concurrent_cnt = 1;
  } else {
    const int64_t macro_block_size = OB_SERVER_BLOCK_MGR.get_macro_block_size();
    if (((macro_block_cnt * macro_block_size + tablet_size - 1) / tablet_size) <= max_merge_thread) {
      concurrent_cnt = (macro_block_cnt * macro_block_size + tablet_size - 1) /
                       tablet_size;
      if (0 == concurrent_cnt) {
        concurrent_cnt = 1;
      }
    } else {
      int64_t macro_cnts = (macro_block_cnt + max_merge_thread - 1) / max_merge_thread;
      concurrent_cnt = (macro_block_cnt + macro_cnts - 1) / macro_cnts;
    }
  }
  return ret;
}

int ObParallelMergeCtx::get_major_parallel_ranges(
    const blocksstable::ObSSTable *first_major_sstable,
    const int64_t tablet_size,
    const ObTableReadInfo &index_read_info)
{
  int ret = OB_SUCCESS;
  int64_t macro_block_cnt = 0;
  if (OB_ISNULL(first_major_sstable)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "major sstable is unexpected null", K(ret), KPC(first_major_sstable));
  } else if (OB_UNLIKELY(concurrent_cnt_ <= 1 || concurrent_cnt_ > MAX_MERGE_THREAD)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "concurrent cnt is invalid", K(ret), K_(concurrent_cnt));
  } else {
    const int64_t macro_block_cnt = first_major_sstable->get_meta().get_macro_info().get_data_block_ids().count();
    const int64_t macro_block_cnt_per_range = (macro_block_cnt + concurrent_cnt_ - 1) / concurrent_cnt_;

    ObDatumRowkey macro_endkey;
    ObDatumRange range;
    range.end_key_.set_min_rowkey();
    range.set_left_open();
    range.set_right_closed();

    blocksstable::ObDataMacroBlockMeta blk_meta;
    blocksstable::ObSSTableSecMetaIterator *meta_iter = nullptr;
    ObDatumRange query_range;
    query_range.set_whole_range();
    if (OB_FAIL(first_major_sstable->scan_secondary_meta(allocator_, query_range,
        index_read_info, DATA_BLOCK_META, meta_iter))) {
      STORAGE_LOG(WARN, "Failed to scan secondary meta", KR(ret), KPC(this));
    }
    // generate ranges
    for (int64_t i = 0; OB_SUCC(ret) && i < macro_block_cnt;) {
      int64_t last = i + macro_block_cnt_per_range - 1;
      last = (last < macro_block_cnt ? last : macro_block_cnt - 1);
      // locate to the last macro-block meta in current range
      while (OB_SUCC(meta_iter->get_next(blk_meta)) && i++ < last);
      if (OB_FAIL(ret)) {
        STORAGE_LOG(WARN, "Failed to get macro block meta", KR(ret), K(i - 1));
      } else if (OB_UNLIKELY(!blk_meta.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "Unexpected invalid macro block meta", KR(ret), K(i - 1));
      } else if (OB_FAIL(blk_meta.get_rowkey(macro_endkey))) {
        STORAGE_LOG(WARN, "Failed to get rowkey", KR(ret), K(blk_meta));
      } else if (OB_FAIL(range.end_key_.deep_copy(range.start_key_, allocator_))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", KR(ret), K(range.get_end_key()), K(range.get_start_key()));
      } else if (OB_FAIL(macro_endkey.deep_copy(range.end_key_, allocator_))) {
        STORAGE_LOG(WARN, "Failed to deep copy rowkey", KR(ret), K(macro_endkey), K(range.get_end_key()), K(range.get_start_key()));
      } else if (OB_FAIL(range_array_.push_back(range))) {
        STORAGE_LOG(WARN, "Failed to push range", KR(ret), K(range_array_), K(range));
      }
    }

    if (OB_NOT_NULL(meta_iter)) {
      meta_iter->~ObSSTableSecMetaIterator();
      allocator_.free(meta_iter);
    }

    if (OB_FAIL(ret)) {
    } else if (OB_UNLIKELY(concurrent_cnt_ != range_array_.count())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "range array size is not equal to concurrent_cnt", K(ret), KPC(this));
    } else {
      ObDatumRange &last_range = range_array_.at(range_array_.count() - 1);
      last_range.end_key_.set_max_rowkey();
      last_range.set_right_open();
    }
  }

  return ret;
}

}
}
