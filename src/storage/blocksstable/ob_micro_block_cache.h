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

#ifndef OCEANBASE_STORAGE_BLOCKSSTABLE_MICRO_BLOCK_CACHE_H_
#define OCEANBASE_STORAGE_BLOCKSSTABLE_MICRO_BLOCK_CACHE_H_
#include "share/io/ob_io_manager.h"
#include "share/cache/ob_kv_storecache.h"
#include "ob_block_sstable_struct.h"
#include "ob_index_block_row_scanner.h"
#include "ob_macro_block_reader.h"
#include "storage/ob_i_table.h"
#include "storage/blocksstable/ob_micro_block_info.h"
#include "storage/meta_mem/ob_tablet_handle.h"

namespace oceanbase
{
namespace blocksstable
{
class ObMicroBlockCacheKey : public common::ObIKVCacheKey
{
public:
  ObMicroBlockCacheKey(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const int64_t offset,
      const int64_t size);
  ObMicroBlockCacheKey(
      const uint64_t tenant_id,
      const ObMicroBlockId &block_id);
  ObMicroBlockCacheKey();
  ObMicroBlockCacheKey(const ObMicroBlockCacheKey &other);
  virtual ~ObMicroBlockCacheKey();
  virtual bool operator ==(const ObIKVCacheKey &other) const;
  virtual uint64_t get_tenant_id() const;
  virtual uint64_t hash() const;
  virtual int64_t size() const;
  virtual int deep_copy(char *buf, const int64_t buf_len, ObIKVCacheKey *&key) const;
  void set(const uint64_t tenant_id,
           const MacroBlockId &block_id,
           const int64_t offset,
           const int64_t size);
  TO_STRING_KV(K_(tenant_id), K_(block_id));
private:
  uint64_t tenant_id_;
  ObMicroBlockId block_id_;
};

class ObMicroBlockCacheValue : public common::ObIKVCacheValue
{
public:
  ObMicroBlockCacheValue(
      const char *buf,
      const int64_t size,
      const char *extra_buf = NULL,
      const int64_t extra_size = 0,
      const ObMicroBlockData::Type block_type = ObMicroBlockData::DATA_BLOCK);
  virtual ~ObMicroBlockCacheValue();
  virtual int64_t size() const;
  virtual int deep_copy(char *buf, const int64_t buf_len, ObIKVCacheValue *&value) const;
  inline const ObMicroBlockData& get_block_data() const { return block_data_; }
  inline ObMicroBlockData& get_block_data() { return block_data_; }
  TO_STRING_KV(K_(block_data));
private:
  ObMicroBlockData block_data_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObMicroBlockCacheValue);
};

class ObIMicroBlockCache;

class ObMicroBlockBufferHandle
{
public:
  ObMicroBlockBufferHandle() : micro_block_(NULL) {}
  ~ObMicroBlockBufferHandle() {}
  void reset() { micro_block_ = NULL; handle_.reset(); }
  inline const ObMicroBlockData* get_block_data() const
  { return is_valid() ? &(micro_block_->get_block_data()) : NULL; }
  inline bool is_valid() const { return NULL != micro_block_ && handle_.is_valid(); }
  TO_STRING_KV(K_(handle), KP_(micro_block));
private:
  friend class ObIMicroBlockCache;
  common::ObKVCacheHandle handle_;
  const ObMicroBlockCacheValue *micro_block_;
};

struct ObMultiBlockIOResult
{
  ObMultiBlockIOResult();
  virtual ~ObMultiBlockIOResult();

  int get_block_data(const int64_t index, ObMicroBlockData &block_data) const;
  void reset();
  const ObMicroBlockCacheValue **micro_blocks_;
  common::ObKVCacheHandle *handles_;
  int64_t block_count_;
  int ret_code_;
};

struct ObMultiBlockIOParam
{
  ObMultiBlockIOParam() { reset(); }
  virtual ~ObMultiBlockIOParam() {}
  void reset();
  bool is_valid() const;
  inline void get_io_range(int64_t &offset, int64_t &size) const;
  inline int get_block_des_info(
      ObMicroBlockDesMeta &des_meta,
      common::ObRowStoreType &row_store_type) const;
  TO_STRING_KV(KPC(micro_index_infos_), K_(start_index), K_(block_count));
  common::ObIArray<ObMicroIndexInfo> *micro_index_infos_;
  int64_t start_index_;
  int64_t block_count_;
};

struct ObMultiBlockIOCtx
{
  ObMultiBlockIOCtx()
    : micro_index_infos_(nullptr), hit_cache_bitmap_(nullptr), block_count_(0) {}
  virtual ~ObMultiBlockIOCtx() {}
  void reset();
  bool is_valid() const;
  ObMicroIndexInfo *micro_index_infos_;
  bool *hit_cache_bitmap_;
  int64_t block_count_;
  TO_STRING_KV(KP_(micro_index_infos), KP_(hit_cache_bitmap), K_(block_count));
};

class ObIPutSizeStat
{
public:
  virtual int add_put_size(const int64_t put_size) = 0;
};

class ObIMicroBlockCache : public ObIPutSizeStat
{
public:
  typedef common::ObIKVCache<ObMicroBlockCacheKey, ObMicroBlockCacheValue> BaseBlockCache;
  int get_cache_block(
      const uint64_t tenant_id,
      const MacroBlockId block_id,
      const int64_t offset,
      const int64_t size,
      ObMicroBlockBufferHandle &handle);
  virtual int prefetch(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const ObMicroIndexInfo& idx_row,
      const common::ObQueryFlag &flag,
      const ObTableReadInfo &full_read_info,
      const ObTabletHandle &tablet_handle,
      ObMacroBlockHandle &macro_handle);
  virtual int load_block(
      const ObMicroBlockId &micro_block_id,
      const ObMicroBlockDesMeta &des_meta,
      const ObTableReadInfo *read_info,
      ObMacroBlockReader *macro_reader,
      ObMicroBlockData &block_data,
      ObIAllocator *allocator);
  virtual void destroy() = 0;
  virtual int get_cache(BaseBlockCache *&cache) = 0;
  virtual int get_allocator(common::ObIAllocator *&allocator) = 0;
  virtual int add_put_size(const int64_t put_size) override;
public:
  // New Block IO Callbacks for version 4.0
  class ObIMicroBlockIOCallback : public common::ObIOCallback
  {
    public:
    ObIMicroBlockIOCallback();
    virtual ~ObIMicroBlockIOCallback();
    virtual int alloc_io_buf(char *&io_buf, int64_t &io_buf_size, int64_t &aligned_offset);
    VIRTUAL_TO_STRING_KV(KP_(io_buffer));
  protected:
    friend class ObIMicroBlockCache;
    int process_block(
        ObMacroBlockReader *reader,
        char *buffer,
        const int64_t offset,
        const int64_t size,
        const ObMicroBlockCacheValue *&micro_block,
        common::ObKVCacheHandle &handle);
    int assign(const ObIMicroBlockIOCallback &other);
    static int cache_decoders(
        const ObColDescIArray &full_col_descs,
        const int64_t data_length,
        ObMicroBlockData &micro_data,
        char *block_buf);
    static int transform_index_block(
        const ObTableReadInfo &index_read_info,
        const int64_t data_length,
        ObMicroBlockData &micro_data,
        char *block_buf,
        ObIndexBlockDataTransformer &transformer);
  private:
    int read_block_and_copy(
        ObMacroBlockReader &reader,
        char *buffer,
        const int64_t size,
        ObMicroBlockData &block_data,
        const ObMicroBlockCacheValue *&micro_block,
        common::ObKVCacheHandle &handle);
    virtual int write_extra_buf_on_demand(
        const int64_t data_length,
        ObMicroBlockData &micro_data,
        char *block_buf) = 0;
    virtual int64_t calc_value_size(int64_t data_length, int64_t row_count) = 0;
    virtual ObMicroBlockData::Type get_type() = 0;

    static const int64_t ALLOC_BUF_RETRY_INTERVAL = 100 * 1000;
    static const int64_t ALLOC_BUF_RETRY_TIMES = 3;
  protected:
    BaseBlockCache *cache_;
    ObIPutSizeStat *put_size_stat_;
    common::ObIAllocator *allocator_;
    char *io_buffer_;
    char *data_buffer_;
    uint64_t tenant_id_;
    MacroBlockId block_id_;
    int64_t offset_;
    int64_t size_;
    ObRowStoreType row_store_type_;
    ObMicroBlockDesMeta block_des_meta_;
    bool use_block_cache_;
  };
protected:
  virtual int prefetch(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const ObIndexBlockRowHeader& idx_row_header,
      const common::ObQueryFlag &flag,
      ObMacroBlockHandle &macro_handle,
      ObIMicroBlockIOCallback &callback);
  virtual int prefetch(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const ObMultiBlockIOParam &io_param,
      const ObQueryFlag &flag,
      ObMacroBlockHandle &macro_handle,
      ObIMicroBlockIOCallback &callback);
};

class ObDataMicroBlockCache
  : public common::ObKVCache<ObMicroBlockCacheKey, ObMicroBlockCacheValue>,
    public ObIMicroBlockCache
{
public:
  ObDataMicroBlockCache() {}
  virtual ~ObDataMicroBlockCache() {}
  int init(const char *cache_name, const int64_t priority = 1);
  virtual void destroy() override;
  int prefetch(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const ObMicroIndexInfo& idx_row,
      const common::ObQueryFlag &flag,
      const ObTableReadInfo &full_read_info,
      const ObTabletHandle &tablet_handle,
      ObMacroBlockHandle &macro_handle) override;
  int prefetch(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const ObMultiBlockIOParam &io_param,
      const ObQueryFlag &flag,
      const ObTableReadInfo &full_read_info,
      ObMacroBlockHandle &macro_handle);
  int load_block(
      const ObMicroBlockId &micro_block_id,
      const ObMicroBlockDesMeta &des_meta,
      const ObTableReadInfo *read_info,
      ObMacroBlockReader *macro_reader,
      ObMicroBlockData &block_data,
      ObIAllocator *allocator) override;
  virtual int get_cache(BaseBlockCache *&cache) override;
  virtual int get_allocator(common::ObIAllocator *&allocator) override;
public:
  class ObDataMicroBlockIOCallback : public ObIMicroBlockIOCallback
  {
  public:
    ObDataMicroBlockIOCallback();
    virtual ~ObDataMicroBlockIOCallback();
    virtual int64_t size() const;
    virtual int inner_process(const bool is_success) override;
    virtual int inner_deep_copy(
        char *buf, const int64_t buf_len,
        ObIOCallback *&callback) const override;
    virtual const char *get_data() override;
    INHERIT_TO_STRING_KV("ObIMicroBlockIOCallback", ObIMicroBlockIOCallback,
        KPC(full_cols_), KP_(micro_block), K_(handle), K_(need_write_extra_buf));
  private:
    virtual int64_t calc_value_size(int64_t data_length, int64_t row_count) override;
    virtual int write_extra_buf_on_demand(
        const int64_t data_length,
        ObMicroBlockData &micro_data,
        char *block_buf) override;
    virtual ObMicroBlockData::Type get_type() override;
  private:
    friend class ObDataMicroBlockCache;
    // Notice: lifetime shoule be longer than AIO or deep copy here
    const ObColDescIArray *full_cols_;
    const ObMicroBlockCacheValue *micro_block_;
    ObTabletHandle tablet_handle_;
    common::ObKVCacheHandle handle_;
    bool need_write_extra_buf_;
  };
  class ObMultiDataBlockIOCallback : public ObIMicroBlockIOCallback
  {
  public:
    ObMultiDataBlockIOCallback();
    virtual ~ObMultiDataBlockIOCallback();
    virtual int64_t size() const;
    virtual int inner_process(const bool is_success) override;
    virtual int inner_deep_copy(
        char *buf, const int64_t buf_len,
        ObIOCallback *&callback) const override;
    virtual const char *get_data() override;
    INHERIT_TO_STRING_KV("ObIMicroBlockIOCallback", ObIMicroBlockIOCallback,
        KPC(full_cols_), K_(io_ctx));
  private:
    virtual int write_extra_buf_on_demand(
        const int64_t data_length,
        ObMicroBlockData &micro_data,
        char *block_buf) override;
    virtual int64_t calc_value_size(int64_t data_length, int64_t row_count) override;
    virtual ObMicroBlockData::Type get_type() override;
  private:
    friend class ObDataMicroBlockCache;
    int set_io_ctx(const ObMultiBlockIOParam &io_param);
    void reset_io_ctx() { io_ctx_.reset(); }
    int deep_copy_ctx(const ObMultiBlockIOCtx &io_ctx);
    int alloc_result();
    void free_result();
    // Notice: lifetime shoule be longer than AIO or deep copy here
    const ObColDescIArray *full_cols_;
    ObMultiBlockIOCtx io_ctx_;
    ObMultiBlockIOResult io_result_;
  };
private:
  common::ObConcurrentFIFOAllocator allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObDataMicroBlockCache);
};

class ObIndexMicroBlockCache
  : public common::ObKVCache<ObMicroBlockCacheKey, ObMicroBlockCacheValue>,
    public ObIMicroBlockCache
{
public:
  ObIndexMicroBlockCache() {}
  virtual ~ObIndexMicroBlockCache() {}
  int init(const char *cache_name, const int64_t priority = 10);
  virtual void destroy() override;
  int prefetch(
      const uint64_t tenant_id,
      const MacroBlockId &macro_id,
      const ObMicroIndexInfo& idx_row,
      const common::ObQueryFlag &flag,
      const ObTableReadInfo &index_read_info,
      const ObTabletHandle &tablet_handle,
      ObMacroBlockHandle &macro_handle) override;
  int load_block(
      const ObMicroBlockId &micro_block_id,
      const ObMicroBlockDesMeta &des_meta,
      const ObTableReadInfo *read_info,
      ObMacroBlockReader *macro_reader,
      ObMicroBlockData &block_data,
      ObIAllocator *allocator) override;
  virtual int get_cache(BaseBlockCache *&cache) override;
  virtual int get_allocator(common::ObIAllocator *&allocator) override;
public:
  class ObIndexMicroBlockIOCallback : public ObIMicroBlockIOCallback
  {
  public:
    ObIndexMicroBlockIOCallback();
    virtual ~ObIndexMicroBlockIOCallback();
    virtual int64_t size() const;
    virtual int inner_process(const bool is_success) override;
    virtual int inner_deep_copy(
        char *buf, const int64_t buf_len,
        ObIOCallback *&callback) const override;
    virtual const char *get_data() override;
    INHERIT_TO_STRING_KV("ObIMicroBlockIOCallback", ObIMicroBlockIOCallback,
        KPC(index_read_info_), KP_(micro_block), K_(handle));
  private:
    virtual int64_t calc_value_size(int64_t data_length, int64_t row_count) override;
    virtual int write_extra_buf_on_demand(
        const int64_t data_length,
        ObMicroBlockData &micro_data,
        char *block_buf) override;
    virtual ObMicroBlockData::Type get_type() override;
  private:
    friend class ObIndexMicroBlockCache;
    // Notice: lifetime shoule be longer than AIO or deep copy here
    const ObTableReadInfo *index_read_info_;
    const ObMicroBlockCacheValue *micro_block_;
    ObTabletHandle tablet_handle_;
    common::ObKVCacheHandle handle_;
  };
private:
  common::ObConcurrentFIFOAllocator allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObIndexMicroBlockCache);
};

}//end namespace blocksstable
}//end namespace oceanbase
#endif
