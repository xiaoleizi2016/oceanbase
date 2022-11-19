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

#include "ob_archive_allocator.h"
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "ob_archive_task.h"           // ObArchiveLogFetchTask ObArchiveSendTask
#include "share/ob_ls_id.h"            // ObLSID
#include "ob_archive_task_queue.h"     // ObArchiveTaskStatus

namespace oceanbase
{
namespace archive
{
using namespace oceanbase::share;
ObArchiveAllocator::ObArchiveAllocator() :
  inited_(false),
  log_fetch_task_allocator_(),
  send_task_allocator_(),
  log_handle_allocator_(),
  send_task_status_allocator_()
{}

ObArchiveAllocator::~ObArchiveAllocator()
{
  destroy();
}

int ObArchiveAllocator::init(const uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  const int64_t clog_task_size = sizeof(ObArchiveLogFetchTask);
  const int64_t send_task_size = sizeof(ObArchiveSendTask);
  const int64_t send_task_status_size = sizeof(ObArchiveTaskStatus);
  const int64_t UNUSED_HOLD_LIMIT = 0;

  if (OB_UNLIKELY(inited_)) {
    ret = OB_INIT_TWICE;
    ARCHIVE_LOG(WARN, "ObArchiveAllocator has been inited", K(ret));
  } else if (OB_INVALID_TENANT_ID == tenant_id) {
    ret = OB_INVALID_ARGUMENT;
    ARCHIVE_LOG(WARN, "invalid argument", K(ret), K(tenant_id));
  } else if (OB_FAIL(log_fetch_task_allocator_.init(clog_task_size, "ArcFetchTask", tenant_id))) {
    ARCHIVE_LOG(WARN, "clog_task_allocator_ init fail", K(ret));
  } else if (OB_FAIL(send_task_allocator_.init(8 * 1024L,    // page_size
                                               "ArcSendTask",    // label
                                               tenant_id,      // tenant_id
                                               1024 * 1024 * 1024L   /* TODO limit */))) {
    // Note: 如果日志流太多, 可能导致内存不够导致的不能work
    ARCHIVE_LOG(WARN, "send_task_allocator_ init failed", K(ret));
  } else if (OB_FAIL(log_handle_allocator_.init(8 * 1024L,    // page_size
                                                "ArcLogHandle",    // label
                                                tenant_id,      // tenant_id
                                                1024 * 1024 * 1024L   /* TODO limit */))) {
  } else if (OB_FAIL(send_task_status_allocator_.init(send_task_status_size, "ArcSendQueue", tenant_id))) {
    ARCHIVE_LOG(WARN, "clog_task_status_allocator_ init fail", K(ret));
  } else {
    inited_ = true;
  }
  return ret;
}

void ObArchiveAllocator::destroy()
{
  if (inited_) {
    (void)log_fetch_task_allocator_.destroy();
    (void)send_task_allocator_.destroy();
    (void)log_handle_allocator_.destroy();
    (void)send_task_status_allocator_.destroy();
    inited_ = false;
  }
}

ObArchiveLogFetchTask *ObArchiveAllocator::alloc_log_fetch_task()
{
  void *data = NULL;
  ObArchiveLogFetchTask *task = NULL;

  if (OB_UNLIKELY(! inited_)) {
    ARCHIVE_LOG(WARN, "ObArchiveAllocator not init");
  } else if (OB_ISNULL(data = log_fetch_task_allocator_.alloc())) {
    // alloc fail
  } else {
    task = new (data) ObArchiveLogFetchTask();
  }
  return task;
}

void ObArchiveAllocator::free_log_fetch_task(ObArchiveLogFetchTask *task)
{
  if (NULL != task) {
    if (NULL != task->get_send_task()) {
      free_send_task(task->get_send_task());
      task->clear_send_task();
    }
    task->~ObArchiveLogFetchTask();
    log_fetch_task_allocator_.free(task);
    task = NULL;
  }
}

ObArchiveSendTask *ObArchiveAllocator::alloc_send_task(const int64_t buf_len)
{
  char *data = NULL;
  ObArchiveSendTask *task = NULL;
  const int64_t size = sizeof(ObArchiveSendTask);

  if (OB_UNLIKELY(! inited_)) {
    ARCHIVE_LOG(WARN, "ObArchiveAllocator not init");
  } else if (OB_ISNULL(data = static_cast<char *>(send_task_allocator_.alloc(size + buf_len)))) {
    // alloc fail
  } else {
    task = new (data) ObArchiveSendTask();
    task->set_buffer(data + size, buf_len);
  }
  return task;
}

void ObArchiveAllocator::free_send_task(ObArchiveSendTask *task)
{
  if (NULL != task) {
    task->~ObArchiveSendTask();
    send_task_allocator_.free(task);
    task = NULL;
  }
}

void *ObArchiveAllocator::alloc_log_handle_buffer(const int64_t size)
{
  return log_handle_allocator_.alloc(size);
}

void ObArchiveAllocator::free_log_handle_buffer(char *buf)
{
  if (NULL != buf) {
    log_handle_allocator_.free(buf);
    buf = NULL;
  }
}

ObArchiveTaskStatus *ObArchiveAllocator::alloc_send_task_status(const share::ObLSID &id)
{
  void *data = NULL;
  ObArchiveTaskStatus *task_status = NULL;

  if (OB_UNLIKELY(! inited_)) {
    ARCHIVE_LOG(WARN, "ObArchiveAllocator not init");
  } else if (OB_ISNULL(data = send_task_status_allocator_.alloc())) {
    ARCHIVE_LOG(WARN, "alloc data fail");
  } else {
    task_status = new (data) ObArchiveTaskStatus(id);
  }

  return task_status;
}

void ObArchiveAllocator::free_send_task_status(ObArchiveTaskStatus *status)
{
  if (NULL != status) {
    status->~ObArchiveTaskStatus();
    send_task_status_allocator_.free(status);
    status = NULL;
  }
}
} // namespace archive
} // namespace oceanbase
