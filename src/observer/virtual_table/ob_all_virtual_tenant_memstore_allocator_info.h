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

#ifndef OB_ALL_VIRTUAL_TENANT_MEMSOTRE_ALLOCATOR_INFO_H_
#define OB_ALL_VIRTUAL_TENANT_MEMSOTRE_ALLOCATOR_INFO_H_

#include "share/ob_virtual_table_iterator.h"
#include "common/ob_range.h"

namespace oceanbase
{
namespace common
{
class ObMemstoreAllocatorMgr;
}
namespace observer
{
struct ObMemstoreAllocatorInfo
{
  ObMemstoreAllocatorInfo()
      : protection_clock_(INT64_MAX),
        is_active_(false),
        ls_id_(OB_INVALID_ID),
        tablet_id_(OB_INVALID_ID),
        log_ts_range_() {}
  ~ObMemstoreAllocatorInfo() {}
  TO_STRING_KV(K_(protection_clock), K_(is_active), K_(ls_id), K_(tablet_id), K_(log_ts_range));
  int64_t protection_clock_;
  bool is_active_;
  int64_t ls_id_;
  uint64_t tablet_id_;
  common::ObLogTsRange log_ts_range_;
};
class ObAllVirtualTenantMemstoreAllocatorInfo : public common::ObVirtualTableIterator
{
public:
  typedef ObMemstoreAllocatorInfo MemstoreInfo;
  ObAllVirtualTenantMemstoreAllocatorInfo();
  virtual ~ObAllVirtualTenantMemstoreAllocatorInfo();
  virtual int inner_open();
  virtual void reset();
  virtual int inner_get_next_row(common::ObNewRow *&row);
private:
  enum COLUMNS
  {
    SVR_IP = common::OB_APP_MIN_COLUMN_ID,
    SVR_PORT,
    TENANT_ID,
    LS_ID,
    TABLET_ID,
    START_TS,
    END_TS,
    IS_ACTIVE,
    RETIRE_CLOCK,
    PROTECTION_CLOCK,
  };
  int fill_tenant_ids();
  int fill_memstore_infos(const uint64_t tenant_id);
  common::ObMemstoreAllocatorMgr &allocator_mgr_;
  common::ObArray<uint64_t> tenant_ids_;
  common::ObArray<MemstoreInfo> memstore_infos_;
  int64_t memstore_infos_idx_;
  int64_t tenant_ids_idx_;
  int64_t col_count_;
  int64_t retire_clock_;
  DISALLOW_COPY_AND_ASSIGN(ObAllVirtualTenantMemstoreAllocatorInfo);
};

}
}

#endif // OB_ALL_VIRTUAL_TENANT_MEMSOTRE_ALLOCATOR_INFO_H_
