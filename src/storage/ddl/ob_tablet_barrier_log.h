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

#ifndef SRC_STORAGE_OB_TABLET_BARRIER_LOG_H_
#define SRC_STORAGE_OB_TABLET_BARRIER_LOG_H_

#include "lib/utility/ob_print_utils.h"
#include "logservice/palf/scn.h"

namespace oceanbase
{
namespace storage
{
enum ObTabletBarrierLogStateEnum
{
  TABLET_BARRIER_LOG_INIT = 0,
  TABLET_BARRIER_LOG_WRITTING,
  TABLET_BARRIER_SOURCE_LOG_WRITTEN,
  TABLET_BARRIER_DEST_LOG_WRITTEN
};

struct ObTabletBarrierLogState final
{
public:
  ObTabletBarrierLogState();
  ~ObTabletBarrierLogState() = default;

  ObTabletBarrierLogStateEnum &get_state() { return state_; }
  palf::SCN get_scn() const { return scn_; }
  int64_t get_schema_version() const { return schema_version_; }

  void reset();
  void set_log_info(
      const ObTabletBarrierLogStateEnum state,
      const palf::SCN &scn,
      const int64_t schema_version);
  NEED_SERIALIZE_AND_DESERIALIZE;
  TO_STRING_KV(K_(state));
private:
  ObTabletBarrierLogStateEnum to_persistent_state() const;
private:
  ObTabletBarrierLogStateEnum state_;
  palf::SCN scn_;
  int64_t schema_version_;
};
}
}

#endif /* SRC_STORAGE_OB_TABLET_BARRIER_LOG_H_ */
