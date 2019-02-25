//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <algorithm>
#include <string>
#include "db/lookup_key.h"
#include "db/merge_context.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/types.h"
#include "util/autovector.h"

namespace rocksdb {
class GetContext;

struct KeyContext {
  const Slice* key;
  ColumnFamilyHandle* column_family;
  LookupKey* lkey;
  Slice ukey;
  Slice ikey;
  Status* s;
  MergeContext merge_context;
  SequenceNumber max_covering_tombstone_seq;
  bool key_exists;
  SequenceNumber seq;
  void* cb_arg;
  PinnableSlice* value;
  GetContext* get_context;

  KeyContext(const Slice& user_key, ColumnFamilyHandle* col_family,
             PinnableSlice* val, Status* stat)
      : key(&user_key),
        column_family(col_family),
        lkey(nullptr),
        s(stat),
        max_covering_tombstone_seq(0),
        key_exists(false),
        seq(0),
        cb_arg(nullptr),
        value(val),
        get_context(nullptr) {}

  KeyContext() = default;
};

// The MultiGetContext class is a container for the sorted list of keys that
// we need to lookup in a batch. Its main purpose is to make batch execution
// easier by allowing various stages of the MultiGet lookups to operate on
// subsets of keys, potentially non-contiguous. In order to accomplish this,
// it defines the following classes -
//
// MultiGetContext::Range - Specifies a range of keys, by start and end index,
// from the parent MultiGetContext. Each range contains a bit vector that
// indicates whether the corresponding keys need to be processed or skipped.
// A Range object can be copy constructed, and the new object inherits the
// original Range's bit vector. This is useful for progressively skipping
// keys as the lookup goes through various stages. For example, when looking
// up keys in the same SST file, a Range is created excluding keys not
// belonging to that file. A new Range is then copy constructed and individual
// keys are skipped based on bloom filter lookup.
//
// MultiGetContext::Range::Iterator - A forward iterator that iterates over
// non-skippable keys in a Range, as well as keys whose final value has been
// found. The latter is tracked by MultiGetContext::value_mask_
// MultiGetContext::Range::IteratorWrapper - A wrapper around a vector
// container, such as std::vector, std::array or autovector, that shadows a
//
// MultiGetContext::Range::Iterator. The size of the vector must be atleast
// the number of keys in the MultiGetContext batch (limited by
// MultiGetContext::MAX_KEYS_ON_STACK), with the indexes of keys being
// identical to that in MultiGetContext. This is useful for maintaining
// auxillary data structures that can be quickly accessed while iterating
// through a MultigetContext::Range. The auxillary data structures can be
// allocated on stack for efficiency
class MultiGetContext {
 public:
  // Limit the number of keys in a batch to this number. Benchmarks show that
  // there is negligible benefit for batches exceeding this. Keeping this < 64
  // simplifies iteration, as well as reduces the amount of stack allocations
  // htat need to be performed
  static const int MAX_KEYS_ON_STACK = 32;

  MultiGetContext(KeyContext** sorted_keys, size_t num_keys,
                  SequenceNumber snapshot)
      : sorted_keys_(sorted_keys),
        num_keys_(num_keys),
        value_mask_(0),
        lookup_key_ptr_(reinterpret_cast<LookupKey*>(lookup_key_buf)) {
    int index = 0;

    for (size_t iter = 0; iter != num_keys_; ++iter) {
      sorted_keys_[iter]->lkey = new (&lookup_key_ptr_[index])
          LookupKey(*sorted_keys_[iter]->key, snapshot);
      sorted_keys_[iter]->ukey = sorted_keys_[iter]->lkey->user_key();
      sorted_keys_[iter]->ikey = sorted_keys_[iter]->lkey->internal_key();
      index++;
    }
  }

  ~MultiGetContext() {
    if (reinterpret_cast<char*>(lookup_key_ptr_) != lookup_key_buf) {
      delete[] lookup_key_ptr_;
    }
  }

 private:
  char lookup_key_buf[sizeof(LookupKey) * MAX_KEYS_ON_STACK];
  KeyContext** sorted_keys_;
  size_t num_keys_;
  uint64_t value_mask_;
  LookupKey* lookup_key_ptr_;

 public:
  class Range {
   public:
    class Iterator {
     public:
      // -- iterator traits
      typedef Iterator self_type;
      typedef KeyContext value_type;
      typedef KeyContext& reference;
      typedef KeyContext* pointer;
      typedef int difference_type;
      typedef std::forward_iterator_tag iterator_category;

      Iterator(const Range* range, size_t index)
          : range_(range), ctx_(range->ctx_), index_(index) {
        while (index_ < range_->end_ &&
               (1ull << index_) &
                   (range_->ctx_->value_mask_ | range_->skip_mask_))
          index_++;
      }

      Iterator(const Iterator&) = default;
      Iterator& operator=(const Iterator&) = default;

      Iterator& operator++() {
        while (++index_ < range_->end_ &&
               (1ull << index_) &
                   (range_->ctx_->value_mask_ | range_->skip_mask_))
          ;
        return *this;
      }

      bool operator==(Iterator other) const {
        assert(range_->ctx_ == other.range_->ctx_);
        return index_ == other.index_;
      }

      bool operator!=(Iterator other) const {
        assert(range_->ctx_ == other.range_->ctx_);
        return index_ != other.index_;
      }

      KeyContext& operator*() {
        assert(index_ < range_->end_ && index_ >= range_->start_);
        return *(ctx_->sorted_keys_[index_]);
      }

      KeyContext* operator->() {
        assert(index_ < range_->end_ && index_ >= range_->start_);
        return ctx_->sorted_keys_[index_];
      }

     private:
      friend Range;
      const Range* range_;
      const MultiGetContext* ctx_;
      size_t index_;
    };

    template <class T>
    class IteratorWrapper {
     public:
      IteratorWrapper(const Range::Iterator& iter, T& vector)
          : iter_(iter), vector_(vector) {}

      typename T::value_type* operator->() { return &vector_[iter_.index_]; }

      typename T::reference operator*() { return vector_[iter_.index_]; }

     private:
      const Range::Iterator& iter_;
      T& vector_;
    };

    Range(const Range& mget_range, const Iterator& first, const Iterator& last) {
      ctx_ = mget_range.ctx_;
      start_ = first.index_;
      end_ = last.index_;
      skip_mask_ = mget_range.skip_mask_;
    }

    Range() = default;

    Iterator begin() const { return Iterator(this, start_); }

    Iterator end() const { return Iterator(this, end_); }

    bool empty() {
      return (((1ull << end_) - 1) & ~((1ull << start_) - 1) &
              ~ctx_->value_mask_ & ~skip_mask_) == 0;
    }

    void SkipKey(const Iterator& iter) { skip_mask_ |= 1ull << iter.index_; }

    // Update the value_mask_ in MultiGetContext so its
    // immediately reflected in all the Range Iterators
    void MarkKeyDone(Iterator& iter) {
      ctx_->value_mask_ |= (1ull << iter.index_);
    }

    bool CheckKeyDone(Iterator& iter) {
      return ctx_->value_mask_ & (1ull << iter.index_);
    }

   private:
    friend MultiGetContext;
    MultiGetContext* ctx_;
    size_t start_;
    size_t end_;
    uint64_t skip_mask_;

    Range(MultiGetContext* ctx, size_t num_keys)
        : ctx_(ctx), start_(0), end_(num_keys), skip_mask_(0) {}
  };

  // Return the initial range that encompasses all the keys in the batch
  Range GetMultiGetRange() { return Range(this, num_keys_); }
};

}  // namespace rocksdb
