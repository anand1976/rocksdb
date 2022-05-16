//  Copyright (c) Meta Platforms, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#if USE_COROUTINES
#include "util/async_file_reader.h"

namespace ROCKSDB_NAMESPACE {
bool AsyncFileReader::MultiReadAsyncImpl(ReadAwaiter* awaiter) {
  if (tail_) {
    tail_->next_ = awaiter;
  }
  tail_ = awaiter;
  if (!head_) {
    head_ = awaiter;
  }
  num_reqs_ += awaiter->num_reqs_;
  awaiter->io_handle_.resize(awaiter->num_reqs_);
  awaiter->del_fn_.resize(awaiter->num_reqs_);
  for (size_t i = 0; i < awaiter->num_reqs_; ++i) {
    awaiter->io_handle_.push_back(nullptr);
    awaiter->del_fn_.push_back(nullptr);
    awaiter->file_
        ->ReadAsync(
            awaiter->read_reqs_[i], awaiter->opts_,
            [](const FSReadRequest& req, void* cb_arg) {
              FSReadRequest* read_req = static_cast<FSReadRequest*>(cb_arg);
              read_req->status = req.status;
              read_req->result = req.result;
            },
            &awaiter->read_reqs_[i], &awaiter->io_handle_[i],
            &awaiter->del_fn_[i], Env::IOPriority::IO_TOTAL)
        .PermitUncheckedError();
  }
  return true;
}

void AsyncFileReader::Wait() {
  if (!head_) {
    return;
  }
  ReadAwaiter* waiter;
  std::vector<void*> io_handles;
  io_handles.reserve(num_reqs_);
  waiter = head_;
  do {
    for (size_t i = 0; i < waiter->num_reqs_; ++i) {
      if (waiter->io_handle_[i]) {
        io_handles.push_back(waiter->io_handle_[i]);
      }
    }
  } while (waiter != tail_ && (waiter = waiter->next_));
  if (io_handles.size() > 0) {
    StopWatch sw(SystemClock::Default().get(), stats_, POLL_WAIT_MICROS);
    fs_->Poll(io_handles, io_handles.size()).PermitUncheckedError();
  }
  do {
    waiter = head_;
    head_ = waiter->next_;

    for (size_t i = 0; i < waiter->num_reqs_; ++i) {
      if (waiter->io_handle_[i] && waiter->del_fn_[i]) {
        waiter->del_fn_[i](waiter->io_handle_[i]);
      }
    }
    waiter->awaiting_coro_.resume();
  } while (waiter != tail_);
  head_ = tail_ = nullptr;
  RecordInHistogram(stats_, MULTIGET_IO_BATCH_SIZE, num_reqs_);
  num_reqs_ = 0;
}
}  // namespace ROCKSDB_NAMESPACE
#endif  // USE_COROUTINES
