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

#define USING_LOG_PREFIX CLOG
#include "lib/ob_errno.h"
#include "share/ob_errno.h"
#include "ob_remote_data_generator.h"
#include "lib/utility/ob_macro_utils.h"
#include "logservice/ob_log_service.h"                    // ObLogService
#include "logservice/palf/log_group_entry.h"              // LogGroupEntry
#include "logservice/archiveservice/ob_archive_file_utils.h"     // ObArchiveFileUtils
#include "share/backup/ob_archive_path.h"           // ObArchivePathUtil
#include "logservice/archiveservice/ob_archive_define.h"         // ObArchiveFileHeader
#include "logservice/archiveservice/ob_archive_util.h"       // ObArchiveFileUtils
#include "share/backup/ob_backup_path.h"                // ObBackupPath
#include "ob_log_restore_rpc.h"                           // proxy
#include "share/backup/ob_backup_struct.h"
#include "share/backup/ob_archive_path.h"           // ObArchivePathUtil

namespace oceanbase
{
namespace logservice
{
using namespace oceanbase::palf;
using namespace oceanbase::share;
using namespace oceanbase::archive;

// ===================================== RemoteDataBuffer ========================== //
void RemoteDataBuffer::reset()
{
  data_ = NULL;
  data_len_ = 0;
  start_lsn_.reset();
  cur_lsn_.reset();
  end_lsn_.reset();
}

int RemoteDataBuffer::set(const LSN &start_lsn, char *data, const int64_t data_len)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! start_lsn.is_valid() || NULL == data || data_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    data_ = data;
    data_len_ = data_len;
    start_lsn_ = start_lsn;
    cur_lsn_ = start_lsn;
    end_lsn_ = start_lsn + data_len;
    ret = set_iterator_();
  }
  return ret;
}

bool RemoteDataBuffer::is_valid() const
{
  return start_lsn_.is_valid()
    && cur_lsn_.is_valid()
    && end_lsn_.is_valid()
    && end_lsn_ > start_lsn_
    && NULL != data_
    && data_len_ > 0;
}

bool RemoteDataBuffer::is_empty() const
{
  return cur_lsn_ == end_lsn_;
}

int RemoteDataBuffer::next(LogGroupEntry &entry, LSN &lsn, char *&buf, int64_t &buf_size)
{
  int ret = OB_SUCCESS;
  if (is_empty()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(iter_.next())) {
    LOG_WARN("next failed", K(ret));
  } else if (OB_FAIL(iter_.get_entry(entry, lsn))) {
    LOG_WARN("get_entry failed", K(ret));
  } else {
    // 当前返回entry对应buff和长度
    buf = data_ + (cur_lsn_ - start_lsn_);
    buf_size = entry.get_serialize_size();
    cur_lsn_ = cur_lsn_ + buf_size;
  }
  return ret;
}

int RemoteDataBuffer::set_iterator_()
{
  int ret = OB_SUCCESS;
  auto get_file_size = [&]() -> LSN { return end_lsn_;};
  if (OB_FAIL(mem_storage_.init(start_lsn_))) {
    LOG_WARN("MemoryStorage init failed", K(ret), K(start_lsn_));
  } else if (OB_FAIL(mem_storage_.append(data_, data_len_))) {
    LOG_WARN("MemoryStorage append failed", K(ret));
  } else if (OB_FAIL(iter_.init(start_lsn_, &mem_storage_, get_file_size))) {
    LOG_WARN("MemPalfGroupBufferIterator init failed", K(ret));
  } else {
    LOG_INFO("MemPalfGroupBufferIterator init succ", K(start_lsn_), K(end_lsn_));
  }
  return ret;
}

// ============================ RemoteDataGenerator ============================ //
RemoteDataGenerator::RemoteDataGenerator(const uint64_t tenant_id,
    const ObLSID &id,
    const LSN &start_lsn,
    const LSN &end_lsn,
    const SCN &end_scn) :
  tenant_id_(tenant_id),
  id_(id),
  start_lsn_(start_lsn),
  next_fetch_lsn_(start_lsn),
  end_scn_(end_scn),
  end_lsn_(end_lsn),
  to_end_(false),
  max_consumed_lsn_(start_lsn)
{}

RemoteDataGenerator::~RemoteDataGenerator()
{
  tenant_id_ = OB_INVALID_TENANT_ID;
  id_.reset();
  start_lsn_.reset();
  max_consumed_lsn_.reset();
  next_fetch_lsn_.reset();
  end_lsn_.reset();
}

bool RemoteDataGenerator::is_valid() const
{
  return OB_INVALID_TENANT_ID != tenant_id_
    && id_.is_valid()
    && start_lsn_.is_valid()
    && end_scn_.is_valid()
    && end_lsn_.is_valid()
    && end_lsn_ > start_lsn_;
}

bool RemoteDataGenerator::is_fetch_to_end() const
{
  return next_fetch_lsn_ >= end_lsn_ || to_end_;
}

int RemoteDataGenerator::update_max_lsn(const palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! lsn.is_valid()
        || (next_fetch_lsn_.is_valid() && next_fetch_lsn_ > lsn))) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(lsn), KPC(this));
  } else {
    next_fetch_lsn_ = lsn;
  }
  return ret;
}
// only handle orignal buffer without compression or encryption
// only to check incomplete LogGroupEntry
// compression and encryption will be supported in the future
//
// 仅支持备份情况下, 不需要处理归档写入不原子情况
int RemoteDataGenerator::process_origin_data_(char *origin_buf,
    const int64_t origin_buf_size,
    char *buf,
    int64_t &buf_size)
{
  UNUSED(origin_buf);
  UNUSED(origin_buf_size);
  UNUSED(buf);
  UNUSED(buf_size);
  return OB_NOT_SUPPORTED;
}
// ================================ ServiceDataGenerator ============================= //
ServiceDataGenerator::ServiceDataGenerator(const uint64_t tenant_id,
    const ObLSID &id,
    const LSN &start_lsn,
    const LSN &end_lsn,
    const SCN &end_scn,
    const ObAddr &server) :
  RemoteDataGenerator(tenant_id, id, start_lsn, end_lsn, end_scn),
  server_(server),
  result_()
{}

ServiceDataGenerator::~ServiceDataGenerator()
{
  server_.reset();
  result_.reset();
}

int ServiceDataGenerator::next_buffer(RemoteDataBuffer &buffer)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ServiceDataGenerator is invalid", K(ret), KPC(this));
  } else if (is_fetch_to_end()) {
    ret = OB_ITER_END;
    LOG_INFO("ServiceDataGenerator to end", K(ret), KPC(this));
  } else if (OB_FAIL(fetch_log_from_net_())) {
    LOG_WARN("fetch_log_from_net_ failed", K(ret), KPC(this));
  } else if (OB_FAIL(buffer.set(start_lsn_, result_.data_, result_.data_len_))) {
    LOG_INFO("buffer set failed", K(ret), KPC(this));
  } else {
    max_consumed_lsn_ = next_fetch_lsn_;
  }
  return ret;
}

bool ServiceDataGenerator::is_valid() const
{
  return RemoteDataGenerator::is_valid()
    && server_.is_valid();
}

// 暂时仅支持单次fetch 需要连续fetch大段数据由上层控制
int ServiceDataGenerator::fetch_log_from_net_()
{
  int ret = OB_SUCCESS;
  ObLogService *log_svr = NULL;
  logservice::ObLogResSvrRpc *proxy = NULL;
  if (OB_ISNULL(log_svr = MTL(logservice::ObLogService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ObLogService is NULL", K(ret), K(log_svr), KPC(this));
  } else if (OB_ISNULL(proxy = log_svr->get_log_restore_service()->get_log_restore_proxy())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ObLogResSvrRpc is NULL", K(ret), K(log_svr), KPC(this));
  } else {
    obrpc::ObRemoteFetchLogRequest req(tenant_id_, id_, start_lsn_, end_lsn_);
    if (OB_FAIL(proxy->fetch_log(server_, req, result_))) {
      LOG_WARN("fetch log failed", K(ret), K(req), KPC(this));
    } else if (OB_UNLIKELY(! result_.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ObRemoteFetchLogResponse is invalid", K(ret), K(req), KPC(this));
    } else {
      to_end_ = true;
      if (result_.is_empty()) {
        ret = OB_ITER_END;
        LOG_INFO("no log exist with the request", K(req), KPC(this));
      } else {
        next_fetch_lsn_ = result_.end_lsn_;
      }
    }
  }
  return ret;
}

// ==================================== LocationDataGenerator ============================== //
// 为加速定位起点文件，依赖LSN -> file_id 规则
static int64_t cal_lsn_to_file_id_(const LSN &lsn)
{
  return cal_archive_file_id(lsn, palf::PALF_BLOCK_SIZE);
}

static int read_file_(const ObString &base,
    const share::ObBackupStorageInfo *storage_info,
    const share::ObLSID &id,
    const int64_t file_id,
    const int64_t offset,
    char *data,
    const int64_t data_len,
    int64_t &data_size)
{
  int ret = OB_SUCCESS;
  share::ObBackupPath path;
  if (OB_FAIL(ObArchivePathUtil::build_restore_path(base.ptr(), id, file_id, path))) {
    LOG_WARN("build restore path failed", K(ret));
  } else {
    ObString uri(path.get_obstr());
    int64_t real_size = 0;
    if (OB_FAIL(ObArchiveFileUtils::range_read(uri, storage_info, data, data_len, offset, real_size))) {
      LOG_WARN("read file failed", K(ret), K(uri), K(storage_info));
    } else if (0 == real_size) {
      ret = OB_ITER_END;
      LOG_INFO("read no data, need retry", K(ret), K(uri), K(storage_info), K(offset), K(real_size));
    } else {
      data_size = real_size;
    }
  }
  return ret;
}

static int extract_archive_file_header_(char *buf,
    const int64_t buf_size,
    palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  archive::ObArchiveFileHeader file_header;
  int64_t pos = 0;
  if (OB_ISNULL(buf) || buf_size <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(buf), K(buf_size));
  } else if (OB_FAIL(file_header.deserialize(buf, buf_size, pos))) {
    LOG_WARN("archive file header deserialize failed", K(ret));
  } else if (OB_UNLIKELY(pos > ARCHIVE_FILE_HEADER_SIZE)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("archive file header size exceed threshold", K(ret), K(pos));
  } else if (OB_UNLIKELY(! file_header.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("invalid file header", K(ret), K(pos), K(file_header));
  } else {
    lsn = LSN(file_header.start_lsn_);
    LOG_INFO("extract_archive_file_header_ succ", K(pos), K(file_header));
  }
  return ret;
}

static int list_dir_files_(const ObString &base,
    const share::ObBackupStorageInfo *storage_info,
    const ObLSID &id,
    int64_t &min_file_id,
    int64_t &max_file_id)
{
  int ret = OB_SUCCESS;
  share::ObBackupPath prefix;
  if (OB_FAIL(ObArchivePathUtil::build_restore_prefix(base.ptr(), id, prefix))) {
  } else {
    ObString uri(prefix.get_obstr());
    ret = ObArchiveFileUtils::get_file_range(uri, storage_info, min_file_id, max_file_id);
  }
  return ret;
}

LocationDataGenerator::LocationDataGenerator(const uint64_t tenant_id,
    const SCN &pre_scn,
    const ObLSID &id,
    const LSN &start_lsn,
    const LSN &end_lsn,
    const SCN &end_scn,
    share::ObBackupDest *dest,
    ObLogArchivePieceContext *piece_context) :
  RemoteDataGenerator(tenant_id, id, start_lsn, end_lsn, end_scn),
  pre_scn_(pre_scn),
  base_lsn_(),
  data_len_(0),
  data_(),
  dest_(dest),
  piece_context_(piece_context),
  dest_id_(0),
  round_id_(0),
  piece_id_(0),
  max_file_id_(-1),
  max_file_offset_(-1)
{}

LocationDataGenerator::~LocationDataGenerator()
{
  pre_scn_.reset();
  base_lsn_.reset();
  dest_ = NULL;
  piece_context_ = NULL;
}

int LocationDataGenerator::next_buffer(RemoteDataBuffer &buffer)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t buf_size = 0;
  if (OB_UNLIKELY(! is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("LocationDataGenerator is invalid", K(ret), KPC(this));
  } else if (is_fetch_to_end()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(fetch_log_from_location_(buf, buf_size))) {
    LOG_WARN("fetch log from location failed", K(ret), KPC(this));
  } else if (OB_FAIL(buffer.set(base_lsn_, buf, buf_size))) {
    LOG_WARN("buffer set failed", K(ret), K(buf), K(buf_size), KPC(this));
  }
  return ret;
}

int LocationDataGenerator::update_max_lsn(const palf::LSN &lsn)
{
  int ret = OB_SUCCESS;
  if (OB_SUCC(RemoteDataGenerator::update_max_lsn(lsn)) && NULL != piece_context_) {
    if (OB_FAIL(piece_context_->update_file_info(dest_id_, round_id_, piece_id_,
            max_file_id_, max_file_offset_, next_fetch_lsn_))) {
      LOG_WARN("piece context update file info failed", K(ret), KPC(this));
    }
  }
  return ret;
}

bool LocationDataGenerator::is_valid() const
{
  return RemoteDataGenerator::is_valid() && NULL != dest_ && NULL != piece_context_;
}

int LocationDataGenerator::fetch_log_from_location_(char *&buf, int64_t &buf_size)
{
  int ret = OB_SUCCESS;
  int64_t file_id = 0;
  int64_t file_offset = 0;
  palf::LSN max_lsn_in_file (palf::LOG_INVALID_LSN_VAL);
  share::ObBackupPath piece_path;
  if (OB_FAIL(get_precise_file_and_offset_(file_id, file_offset, max_lsn_in_file, piece_path))) {
    LOG_WARN("get precise file and offset failed", K(ret));
  } else if (OB_FAIL(read_file_(piece_path.get_ptr(), dest_->get_storage_info(), id_,
          file_id, file_offset, data_, MAX_DATA_BUF_LEN, data_len_))) {
    LOG_WARN("read file failed", K(ret));
  } else if (file_offset > 0) {
    // 非第一次读文件, 不必再解析file header
    base_lsn_ = max_lsn_in_file;
    buf = data_;
    buf_size = data_len_;
  } else if (OB_FAIL(extract_archive_file_header_(data_, data_len_, base_lsn_))) {
    LOG_WARN("extract archive file heaeder failed", K(ret), KPC(this));
  } else {
    buf = data_ + ARCHIVE_FILE_HEADER_SIZE;
    buf_size = data_len_ - ARCHIVE_FILE_HEADER_SIZE;
  }

  if ((OB_SUCC(ret) && base_lsn_ > start_lsn_) || OB_ERR_OUT_OF_LOWER_BOUND == ret) {
    LOG_INFO("read file base_lsn bigger than start_lsn, reset locate info", K(base_lsn_), K(start_lsn_));
    piece_context_->reset_locate_info();
    ret = OB_ITER_END;
  }

  // 更新读取归档文件信息
  if (OB_SUCC(ret)) {
    max_file_id_ = file_id;
    max_file_offset_ = file_offset + data_len_;
  }
  return ret;
}

// 当前起始LSN小于piece_context已消费最大LSN, 需要relocate piece
// 因为以LSN计算file_id, 该file可能位于当前piece, 也可能位于前一个或者多个piece, 需要重新locate
int LocationDataGenerator::get_precise_file_and_offset_(int64_t &file_id,
    int64_t &file_offset,
    palf::LSN &lsn,
    share::ObBackupPath &piece_path)
{
  int ret = OB_SUCCESS;
  if (FALSE_IT(file_id = cal_lsn_to_file_id_(start_lsn_))) {
  } else if (OB_FAIL(piece_context_->get_piece(pre_scn_, start_lsn_, dest_id_, round_id_, piece_id_,
          file_id, file_offset, lsn))) {
    if (OB_ARCHIVE_LOG_TO_END == ret) {
      ret = OB_ITER_END;
    } else {
      LOG_WARN("get cur piece failed", K(ret), KPC(piece_context_));
    }
  } else if (OB_FAIL(share::ObArchivePathUtil::get_piece_dir_path(*dest_, dest_id_, round_id_, piece_id_, piece_path))) {
    LOG_WARN("get piece dir path failed", K(ret));
  } else if (lsn.is_valid() && lsn > start_lsn_) {
    file_offset = 0;
  }

  return ret;
}


// ==================================== RawPathDataGenerator ============================== //
RawPathDataGenerator::RawPathDataGenerator(const uint64_t tenant_id,
    const ObLSID &id,
    const LSN &start_lsn,
    const LSN &end_lsn,
    const DirArray &array,
    const SCN &end_scn,
    const int64_t piece_index,
    const int64_t min_file_id,
    const int64_t max_file_id) :
  RemoteDataGenerator(tenant_id, id, start_lsn, end_lsn, end_scn),
  array_(array),
  data_len_(0),
  base_lsn_(),
  index_(piece_index),
  min_file_id_(min_file_id),
  max_file_id_(max_file_id)
{
}

RawPathDataGenerator::~RawPathDataGenerator()
{
  array_.reset();
  data_len_ = 0;
  base_lsn_.reset();
  index_ = 0;
}

int RawPathDataGenerator::next_buffer(RemoteDataBuffer &buffer)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("RawPathDataGenerator is invalid", K(ret), KPC(this));
  } else if (is_fetch_to_end()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(fetch_log_from_dest_())) {
    LOG_WARN("fetch log from dest failed", K(ret), KPC(this));
  } else if (OB_FAIL(buffer.set(base_lsn_, data_ + ARCHIVE_FILE_HEADER_SIZE, data_len_ - ARCHIVE_FILE_HEADER_SIZE))) {
    LOG_WARN("buffer set failed", K(ret), KPC(this));
  }
  return ret;
}

int RawPathDataGenerator::fetch_log_from_dest_()
{
  int ret = OB_SUCCESS;
  ObString uri(array_[index_].first.ptr());
  share::ObBackupStorageInfo storage_info;
  if (OB_FAIL(storage_info.set(uri.ptr(), array_[index_].second.ptr()))) {
    LOG_WARN("failed to set storage info", K(ret));
  } else if (FALSE_IT(cal_lsn_to_file_id_(start_lsn_))) {
  } else if (OB_FAIL(locate_precise_piece_())) {
    LOG_WARN("locate precise piece failed", K(ret), KPC(this));
  } else if (OB_FAIL(read_file_(uri, &storage_info, file_id_))) {
    LOG_WARN("read file failed", K(ret), K_(id), K_(file_id));
  } else if (OB_FAIL(extract_archive_file_header_())) {
    LOG_WARN("extract archive file header failed", K(ret));
  }
  return ret;
}

// 为加速定位起点文件，依赖LSN -> file_id 规则
int RawPathDataGenerator::cal_lsn_to_file_id_(const LSN &lsn)
{
  //TODO get_archive_file_size from restore config
  file_id_ = cal_archive_file_id(lsn, palf::PALF_BLOCK_SIZE);
  return OB_SUCCESS;
}

int RawPathDataGenerator::list_dir_files_(const ObString &base,
    const share::ObBackupStorageInfo *storage_info,
    int64_t &min_file_id,
    int64_t &max_file_id)
{
  int ret = OB_SUCCESS;
  share::ObBackupPath prefix;
  if (OB_FAIL(ObArchivePathUtil::build_restore_prefix(base.ptr(), id_, prefix))) {
  } else {
    ObString uri(prefix.get_obstr());
    ret = ObArchiveFileUtils::get_file_range(uri, storage_info, min_file_id, max_file_id);
  }
  return ret;
}

int RawPathDataGenerator::read_file_(const ObString &base,
    const share::ObBackupStorageInfo *storage_info,
    const int64_t file_id)
{
  int ret = OB_SUCCESS;
  share::ObBackupPath path;
  if (OB_FAIL(ObArchivePathUtil::build_restore_path(base.ptr(), id_, file_id, path))) {
    LOG_WARN("build restore path failed", K(ret));
  } else {
    ObString uri(path.get_obstr());
    int64_t real_size = 0;
    int64_t file_length = 0;
    if (OB_FAIL(ObArchiveFileUtils::get_file_length(uri, storage_info, file_length))) {
      LOG_WARN("read_file failed", K(ret), K(uri), KP(storage_info));
    } else if (0 == file_length) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("file_length is empty", K(ret), K(uri), KP(storage_info), K(file_length));
    } else if (OB_FAIL(ObArchiveFileUtils::read_file(uri, storage_info, data_, file_length, real_size))) {
      LOG_WARN("read file failed", K(ret), K_(id), K(file_id));
    } else if (0 == real_size) {
      ret = OB_ITER_END;
    } else {
      data_len_ = real_size;
    }
  }
  return ret;
}

int RawPathDataGenerator::extract_archive_file_header_()
{
  int ret = OB_SUCCESS;
  archive::ObArchiveFileHeader file_header;
  int64_t pos = 0;
  if (OB_FAIL(file_header.deserialize(data_, data_len_, pos))) {
    LOG_WARN("archive file header deserialize failed", K(ret), KPC(this));
  } else if (OB_UNLIKELY(pos > ARCHIVE_FILE_HEADER_SIZE)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("archive file header size exceed threshold", K(ret), K(pos), KPC(this));
  } else if (OB_UNLIKELY(! file_header.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("invalid file header", K(ret), K(pos), K(file_header), KPC(this));
  } else {
    base_lsn_ = LSN(file_header.start_lsn_);
    LOG_INFO("extract_archive_file_header_ succ", K(pos), K(file_header), KPC(this));
  }
  return ret;
}

int RawPathDataGenerator::locate_precise_piece_()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(0 == array_.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("dest array count is 0", K(ret), KPC(this));
  } else if (piece_index_match_()) {
  } else {
    int64_t min_file_id = 0;
    int64_t max_file_id = 0;
    bool locate = false;
    for (int64_t i = 0; i < array_.count(); i++) {
      ObString uri(array_[i].first.ptr());
      share::ObBackupStorageInfo storage_info;
      if (OB_FAIL(storage_info.set(uri.ptr(), array_[i].second.ptr()))) {
        LOG_WARN("failed to set storage info", K(ret), KPC(this));
      } else if (OB_FAIL(list_dir_files_(uri, &storage_info, min_file_id, max_file_id))) {
        LOG_WARN("list dir files failed", K(ret), KPC(this));
      } else if (file_id_ >= min_file_id && file_id_ <= max_file_id) {
        locate = true;
        index_ = i;
        min_file_id_ = min_file_id;
        max_file_id_ = max_file_id;
        break;
      }
    }
    if (OB_SUCC(ret) && ! locate) {
      ret = OB_ENTRY_NOT_EXIST;
    }
  }
  return ret;
}

bool RawPathDataGenerator::piece_index_match_() const
{
  return index_ > 0 && min_file_id_ <= file_id_ && max_file_id_ >= file_id_;
}
} // namespace logservice
} // namespace oceanbase
