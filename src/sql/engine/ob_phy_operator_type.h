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

#ifdef PHY_OP_DEF
/* start of phy operator type */
PHY_OP_DEF(PHY_INVALID) /* 0 */
PHY_OP_DEF(PHY_LIMIT)   /* 1 */
PHY_OP_DEF(PHY_SORT)
PHY_OP_DEF(PHY_TABLE_SCAN)
PHY_OP_DEF(PHY_VIRTUAL_TABLE_SCAN)
PHY_OP_DEF(PHY_MERGE_JOIN) /* 5 */
PHY_OP_DEF(PHY_NESTED_LOOP_JOIN)
PHY_OP_DEF(PHY_HASH_JOIN)
PHY_OP_DEF(PHY_MERGE_GROUP_BY)
PHY_OP_DEF(PHY_HASH_GROUP_BY)
PHY_OP_DEF(PHY_SCALAR_AGGREGATE) /* 10 */
PHY_OP_DEF(PHY_MERGE_DISTINCT)
PHY_OP_DEF(PHY_HASH_DISTINCT)  /*not implement yet*/
PHY_OP_DEF(PHY_ROOT_TRANSMIT)
PHY_OP_DEF(PHY_DIRECT_TRANSMIT)
PHY_OP_DEF(PHY_DIRECT_RECEIVE) /* 15 */
PHY_OP_DEF(PHY_DISTRIBUTED_TRANSMIT)
PHY_OP_DEF(PHY_FIFO_RECEIVE)
PHY_OP_DEF(PHY_MERGE_UNION)
PHY_OP_DEF(PHY_HASH_UNION)
PHY_OP_DEF(PHY_MERGE_INTERSECT) /* 20 */
PHY_OP_DEF(PHY_HASH_INTERSECT)
PHY_OP_DEF(PHY_MERGE_EXCEPT)
PHY_OP_DEF(PHY_HASH_EXCEPT)
PHY_OP_DEF(PHY_INSERT)
PHY_OP_DEF(PHY_UPDATE) /* 25 */
PHY_OP_DEF(PHY_DELETE)
PHY_OP_DEF(PHY_REPLACE)
PHY_OP_DEF(PHY_INSERT_ON_DUP)
PHY_OP_DEF(PHY_VALUES)
PHY_OP_DEF(PHY_EXPR_VALUES)   /* 30 */
PHY_OP_DEF(PHY_AUTOINCREMENT)
PHY_OP_DEF(PHY_SUBPLAN_SCAN)
PHY_OP_DEF(PHY_SUBPLAN_FILTER)
PHY_OP_DEF(PHY_MATERIAL)
PHY_OP_DEF(PHY_BLOCK_BASED_NESTED_LOOP_JOIN)  /*35*/
PHY_OP_DEF(PHY_DOMAIN_INDEX)  // no longer used
PHY_OP_DEF(PHY_TABLE_SCAN_WITH_DOMAIN_INDEX) // no-longer used
PHY_OP_DEF(PHY_WINDOW_FUNCTION)
PHY_OP_DEF(PHY_SELECT_INTO)
PHY_OP_DEF(PHY_TOPK)    /*40*/
PHY_OP_DEF(PHY_MV_TABLE_SCAN)
PHY_OP_DEF(PHY_APPEND)
PHY_OP_DEF(PHY_ROOT_RECEIVE)
PHY_OP_DEF(PHY_DISTRIBUTED_RECEIVE)
PHY_OP_DEF(PHY_FIFO_RECEIVE_V2) /*45*/
PHY_OP_DEF(PHY_TASK_ORDER_RECEIVE)
PHY_OP_DEF(PHY_MERGE_SORT_RECEIVE)
PHY_OP_DEF(PHY_NESTED_LOOP_CONNECT_BY_WITH_INDEX)
PHY_OP_DEF(PHY_COUNT)
PHY_OP_DEF(PHY_RECURSIVE_UNION_ALL) /*50*/
PHY_OP_DEF(PHY_FAKE_CTE_TABLE)
PHY_OP_DEF(PHY_MERGE)
PHY_OP_DEF(PHY_ROW_SAMPLE_SCAN)
PHY_OP_DEF(PHY_BLOCK_SAMPLE_SCAN)
PHY_OP_DEF(PHY_INSERT_RETURNING) /*55*/
PHY_OP_DEF(PHY_REPLACE_RETURNING)
PHY_OP_DEF(PHY_INSERT_ON_DUP_RETURNING)
PHY_OP_DEF(PHY_DELETE_RETURNING)
PHY_OP_DEF(PHY_UPDATE_RETURNING)
PHY_OP_DEF(PHY_MULTI_PART_INSERT) /*60*/
PHY_OP_DEF(PHY_MULTI_PART_UPDATE)
PHY_OP_DEF(PHY_MULTI_PART_DELETE)
PHY_OP_DEF(PHY_UK_ROW_TRANSFORM)
PHY_OP_DEF(PHY_DETERMINATE_TASK_TRANSMIT)
PHY_OP_DEF(PHY_MULTI_PART_TABLE_SCAN) /*65*/
PHY_OP_DEF(PHY_TABLE_LOOKUP)
PHY_OP_DEF(PHY_GRANULE_ITERATOR)
PHY_OP_DEF(PHY_PX_FIFO_RECEIVE)
PHY_OP_DEF(PHY_PX_MERGE_SORT_RECEIVE)
PHY_OP_DEF(PHY_PX_DIST_TRANSMIT) /*70*/
PHY_OP_DEF(PHY_PX_REPART_TRANSMIT)
PHY_OP_DEF(PHY_PX_REDUCE_TRANSMIT)
PHY_OP_DEF(PHY_PX_FIFO_COORD)
PHY_OP_DEF(PHY_PX_MERGE_SORT_COORD)
PHY_OP_DEF(PHY_TABLE_ROW_STORE) /*75*/
PHY_OP_DEF(PHY_JOIN_FILTER)
PHY_OP_DEF(PHY_TABLE_CONFLICT_ROW_FETCHER)
PHY_OP_DEF(PHY_MULTI_TABLE_REPLACE)
PHY_OP_DEF(PHY_MULTI_TABLE_INSERT_UP)
PHY_OP_DEF(PHY_SEQUENCE) /*80*/
PHY_OP_DEF(PHY_EXPR_VALUES_WITH_CHILD)
PHY_OP_DEF(PHY_FUNCTION_TABLE)
PHY_OP_DEF(PHY_MONITORING_DUMP)
PHY_OP_DEF(PHY_MULTI_TABLE_MERGE)
PHY_OP_DEF(PHY_LIGHT_GRANULE_ITERATOR) /*85*/
PHY_OP_DEF(PHY_PX_MULTI_PART_DELETE)
PHY_OP_DEF(PHY_PX_MULTI_PART_UPDATE)
PHY_OP_DEF(PHY_PX_MULTI_PART_INSERT)
PHY_OP_DEF(PHY_UNPIVOT)
PHY_OP_DEF(PHY_NESTED_LOOP_CONNECT_BY) /*90*/
PHY_OP_DEF(PHY_LINK)
PHY_OP_DEF(PHY_LOCK)
PHY_OP_DEF(PHY_MULTI_LOCK)
PHY_OP_DEF(PHY_TEMP_TABLE_INSERT)
PHY_OP_DEF(PHY_TEMP_TABLE_ACCESS) /*95*/
PHY_OP_DEF(PHY_TEMP_TABLE_TRANSFORMATION)
PHY_OP_DEF(PHY_MULTI_TABLE_INSERT)
PHY_OP_DEF(PHY_PX_MULTI_PART_SSTABLE_INSERT)
PHY_OP_DEF(PHY_ERR_LOG)
PHY_OP_DEF(PHY_PX_ORDERED_COORD)
PHY_OP_DEF(PHY_STAT_COLLECTOR)
/* end of phy operator type */
PHY_OP_DEF(PHY_NEW_OP_ADAPTER)
PHY_OP_DEF(PHY_FAKE_TABLE)  /* for testing only*/
PHY_OP_DEF(PHY_END)
#endif /*PHY_OP_DEF*/

#ifndef _OB_PHY_OPERATOR_TYPE_H
#define _OB_PHY_OPERATOR_TYPE_H 1
#include <stdint.h>
#include "lib/thread_local/ob_tsi_utils.h"

namespace oceanbase
{
namespace sql
{

/* @note: append only */
  enum ObPhyOperatorType
  {
#define PHY_OP_DEF(type) type,
#include "sql/engine/ob_phy_operator_type.h"
#undef PHY_OP_DEF
  };
  const char *get_phy_op_name(ObPhyOperatorType type);
struct ObPhyOperatorTypeDescSet
{
  struct ObPhyOperatorTypeDesc
  {
    const char *name_;
    ObPhyOperatorTypeDesc() : name_(NULL) {}
  };
  ObPhyOperatorTypeDescSet()
  {
#define PHY_OP_DEF(type) set_type_str(type, #type);
#include "sql/engine/ob_phy_operator_type.h"
#undef PHY_OP_DEF
  }
  void set_type_str(ObPhyOperatorType type, const char *type_str);
  const char *get_type_str(ObPhyOperatorType type) const;
private:
  ObPhyOperatorTypeDesc set_[PHY_END];
};

OB_INLINE bool is_phy_op_type_valid(ObPhyOperatorType type)
{
  return PHY_INVALID < type && type < PHY_END;
}

const char *ob_phy_operator_type_str(ObPhyOperatorType type);
}
}

#endif /* _OB_PHY_OPERATOR_TYPE_H */
