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

#ifndef OCEABASE_LOGSERVICE_SCN_
#define OCEABASE_LOGSERVICE_SCN_
#include "lib/ob_define.h"                      // Serialization
#include "lib/utility/ob_print_utils.h"         // Print*
#include "log_define.h"                         // OB_INVALID_SCN_VAL
namespace oceanbase {
namespace palf {
class SCN
{
public:
  SCN() : ts_ns_(OB_INVALID_SCN_VAL), v_(SCN_VERSION) {}
public:
  void reset();
  bool is_valid() const;
  //convert functions
  // convert scn to timestamp ts
  // @return timestamp with us
  int64_t convert_to_ts() const;
  // convert ts_ns generated by gts to scn. only used by gts
  // @param[in] ts_ns: ts_ns generated by gts
  int convert_for_gts(int64_t ts_ns);
  // convert id generated by lsn allocator. only used by lsn allocator
  // @param[in] id: id generated by lsn allocator
  int convert_for_lsn_allocator(uint64_t id);
  // convert scn_related column value to scn. only used when extracting inner table query result
  // @param[in] column_value: query result of scn_related column of inner table
  int convert_for_inner_table_field(uint64_t column_value);
  //only for filling inner_table fields
  uint64_t get_val_for_inner_table_field() const;
  // compare function
  bool operator==(const SCN &scn) const;
  bool operator!=(const SCN &scn) const;
  bool operator<(const SCN &scn) const;
  bool operator<=(const SCN &scn) const;
  bool operator>(const SCN &scn) const;
  bool operator>=(const SCN &scn) const;
  SCN &operator=(const SCN &scn);
  TO_STRING_KV(K_(val));
  NEED_SERIALIZE_AND_DESERIALIZE;
private:
  static const uint64_t SCN_VERSION = 0;
  union {
    uint64_t val_;
    struct {
      uint64_t ts_ns_ : 62;
      uint64_t v_ : 2;
    };
  };
};

} // end namespace palf
} // end namespace oceanbase
#endif
