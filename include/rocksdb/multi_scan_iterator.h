#pragma once

#include "rocksdb/db.h"
#include "rocksdb/iterator.h"

namespace ROCKSDB_NAMESPACE {

#if 0
// An iterator that returns results from multiple scan ranges. The ranges are
// expected to be in increasing sorted order. The application on top of RocksDB
// would use this as follows -
//
//  std::vector<ScanDesc> scans{{.start = Slice("foo")},
//                              {.start = Slice("bar")}};
//  std::unique_ptr<MultiScanIterator> iter.reset(
//                                      db->NewMultiScanIterator());
//  for (auto& scan : scans) {
//    PinnableSlice val;
//    while (iter-status().ok() && !iter->empty()) {
//      Slice key;
//      std::string value;
//      iter->Dequeue(key, val);
//      if (val.IsPinned()) {
//        val_str = val.ToString();
//      } else {
//        val_str = std::move(*val.GetSelf());
//      }
//      val.Reset();
//      // Do something with key and val_str
//    }
//    if (!iter->status().ok()) {
//      break;
//    }
//    assert(iter->empty());
//    iter->SeekNext();
//  }
//  assert(!iter->status().ok() || iter->empty());
//
class MultiScanIterator {
 public:
  MultiScanIterator(const std::vector<ScanDesc>& scans,
                    std::unique_ptr<Iterator>&& iter)
      : scans_(scans), idx_(0), iter_(std::move(iter)) {
    // Position the iterator for the first scan
    NextScan();
  }

  void Dequeue(Slice& key, PinnableSlice& value) {
    key = iter_->key();
    value.PinSelf(iter_->value());
    iter_->Next();
    if (!iter_->Valid()) {
      empty_ = true;
    }
    status_ = iter_->status();
  }

  void SeekNext() {
    idx_++;
    if (idx_ < scans_.size()) {
      NextScan();
    } else {
      empty_ = true;
    }
  }

  bool empty() { return empty_; }

  Status status() { return status_; }

 private:
  const std::vector<ScanDesc>& scans_;
  size_t idx_;
  std::unique_ptr<Iterator> iter_;
  bool empty_;
  Status status_;

  void NextScan() {
    iter_->Seek(scans_[idx_].start);
    empty_ = !iter_->Valid();
    status_ = iter_->status();
  }
};
#endif

// An iterator that returns results from multiple scan ranges. The ranges are
// expected to be in increasing sorted order. The application on top of RocksDB
// would use this as follows -
//
//  std::vector<ScanDesc> scans{{.start = Slice("bar")},
//                              {.start = Slice("foo")}};
//  std::unique_ptr<MultiScanIterator> iter.reset(
//                                      db->NewMultiScanIterator());
//  try {
//    for (auto scan = iter.begin(); scan != iter.end(); ++scan) {
//      for (auto kv = scan.begin(); kv != scan.end(); ++kv) {
//        auto kvpair = *kv;
//        // Do something with key - kv.first
//        // Do something with value - kv.second
//      }
//    }
//  } catch {
//  }
class MultiScanIterator {
 public:
  MultiScanIterator(const std::vector<ScanDesc>& scans,
                    std::unique_ptr<Iterator>&& db_iter)
      : scans_(scans), db_iter_(std::move(db_iter)) {}

  explicit MultiScanIterator(std::unique_ptr<Iterator>&& db_iter)
    : db_iter_(std::move(db_iter)) {}

  class ScanIterator {
   public:
    class Scan;

    using self_type = ScanIterator;
    using value_type = Scan;
    using reference = Scan&;
    using pointer = Scan*;
    using difference_type = int;
    using iterator_category = std::input_iterator_tag;

    ScanIterator(const std::vector<ScanDesc>& scans, Iterator* db_iter)
        : scans_(scans), idx_(0), db_iter_(db_iter), scan_(db_iter_) {
      db_iter_->Seek(*scans_[idx_].range.start);
      status_ = db_iter_->status();
      if (!status_.ok()) {
        throw status_;
      }
    }

    ScanIterator(const std::vector<ScanDesc>& scans)
        : scans_(scans), idx_(scans_.size()), db_iter_(nullptr), scan_(nullptr) {}

    ScanIterator& operator++() {
      if (idx_ >= scans_.size()) {
        throw Status::InvalidArgument("Index out of range");
      }
      idx_++;
      if (idx_ < scans_.size()) {
        db_iter_->Seek(*scans_[idx_].range.start);
        status_ = db_iter_->status();
        if (!status_.ok()) {
          throw status_;
        }
      }
      return *this;
    }

    bool operator==(ScanIterator other) const { return idx_ == other.idx_; }

    bool operator!=(ScanIterator other) const { return idx_ != other.idx_; }

    reference operator*() { return scan_; }
    reference operator->() { return scan_; }

    class Scan {
     public:
      class SingleIterator;

      Scan(Iterator* db_iter) : db_iter_(db_iter) {}

      SingleIterator begin() { return SingleIterator(db_iter_); }

      SingleIterator end() { return SingleIterator(); }

      class SingleIterator {
       public:
        using self_type = SingleIterator;
        using value_type = std::pair<Slice, Slice>;
        using reference = std::pair<Slice, Slice>&;
        using pointer = std::pair<Slice, Slice>*;
        using difference_type = int;
        using iterator_category = std::input_iterator_tag;

        explicit SingleIterator(Iterator* db_iter) : db_iter_(db_iter) {
          valid_ = db_iter_->Valid();
          if (valid_) {
            result_ = value_type(db_iter_->key(), db_iter_->value());
          }
        }

        SingleIterator() : db_iter_(nullptr), valid_(false) {}

        SingleIterator& operator++() {
          if (!valid_) {
            throw Status::InvalidArgument("Trying to advance invalid iterator");
          } else {
            db_iter_->Next();
            status_ = db_iter_->status();
            if (!status_.ok()) {
              throw status_;
            } else {
              valid_ = db_iter_->Valid();
              if (valid_) {
                result_ = value_type(db_iter_->key(), db_iter_->value());
              }
            }
          }
          return *this;
        }

        bool operator==(SingleIterator other) const {
          return valid_ == other.valid_;
        }

        bool operator!=(SingleIterator other) const {
          return valid_ != other.valid_;
        }

        reference operator*() {
          if (!valid_) {
            throw Status::InvalidArgument("Trying to deref invalid iterator");
          }
          return result_;
        }
        reference operator->() {
          if (!valid_) {
            throw Status::InvalidArgument("Trying to deref invalid iterator");
          }
          return result_;
        }

       private:
        Iterator* db_iter_;
        bool valid_;
        Status status_;
        value_type result_;
      };

     private:
      Iterator* db_iter_;
    };

   private:
    const std::vector<ScanDesc>& scans_;
    size_t idx_;
    Iterator* db_iter_;
    Status status_;
    Scan scan_;
  };

  ScanIterator begin() { return ScanIterator(scans_, db_iter_.get()); }

  ScanIterator end() { return ScanIterator(scans_); }

 private:
  const std::vector<ScanDesc> scans_;
  std::unique_ptr<Iterator> db_iter_;
};

}  // namespace ROCKSDB_NAMESPACE
