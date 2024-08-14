//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#ifndef ROCKSDB_LITE

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/db_iter.h"
#include "db/pre_release_callback.h"
#include "db/read_callback.h"
#include "db/snapshot_checker.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/utilities/transaction_db.h"
#include "util/set_comparator.h"
#include "util/string_util.h"
#include "utilities/transactions/pessimistic_transaction.h"
#include "utilities/transactions/pessimistic_transaction_db.h"
#include "utilities/transactions/transaction_lock_mgr.h"
#include "utilities/transactions/write_prepared_txn.h"

namespace rocksdb {

// A PessimisticTransactionDB that writes data to DB after prepare phase of 2PC.
// In this way some data in the DB might not be committed. The DB provides
// mechanisms to tell such data apart from committed data(write prepared policy，用来区分memtable中未提交和已提交的数据).
class WritePreparedTxnDB : public PessimisticTransactionDB {
 public:
  explicit WritePreparedTxnDB(DB* db,
                              const TransactionDBOptions& txn_db_options)
      : PessimisticTransactionDB(db, txn_db_options),
        SNAPSHOT_CACHE_BITS(txn_db_options.wp_snapshot_cache_bits),
        SNAPSHOT_CACHE_SIZE(static_cast<size_t>(1ull << SNAPSHOT_CACHE_BITS)),
        COMMIT_CACHE_BITS(txn_db_options.wp_commit_cache_bits),
        COMMIT_CACHE_SIZE(static_cast<size_t>(1ull << COMMIT_CACHE_BITS)),
        FORMAT(COMMIT_CACHE_BITS) {
    Init(txn_db_options);
  }

  explicit WritePreparedTxnDB(StackableDB* db,
                              const TransactionDBOptions& txn_db_options)
      : PessimisticTransactionDB(db, txn_db_options),
        SNAPSHOT_CACHE_BITS(txn_db_options.wp_snapshot_cache_bits),
        SNAPSHOT_CACHE_SIZE(static_cast<size_t>(1ull << SNAPSHOT_CACHE_BITS)),
        COMMIT_CACHE_BITS(txn_db_options.wp_commit_cache_bits),
        COMMIT_CACHE_SIZE(static_cast<size_t>(1ull << COMMIT_CACHE_BITS)),
        FORMAT(COMMIT_CACHE_BITS) {
    Init(txn_db_options);
  }

  virtual ~WritePreparedTxnDB();

  virtual Status Initialize(
      const std::vector<size_t>& compaction_enabled_cf_indices,
      const std::vector<ColumnFamilyHandle*>& handles) override;

  Transaction* BeginTransaction(const WriteOptions& write_options,
                                const TransactionOptions& txn_options,
                                Transaction* old_txn) override;

  // Optimized version of ::Write that receives more optimization request such
  // as skip_concurrency_control.
  using PessimisticTransactionDB::Write;
  Status Write(const WriteOptions& opts, const TransactionDBWriteOptimizations&,
               WriteBatch* updates) override;

  // Write the batch to the underlying DB and mark it as committed. Could be
  // used by both directly from TxnDB or through a transaction.
  Status WriteInternal(const WriteOptions& write_options, WriteBatch* batch,
                       size_t batch_cnt, WritePreparedTxn* txn);

  using DB::Get;
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     PinnableSlice* value) override;

  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys,
      std::vector<std::string>* values) override;

  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family) override;

  using DB::NewIterators;
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_families,
      std::vector<Iterator*>* iterators) override;

  // Check whether the transaction that wrote the value with sequence number seq
  // is visible to the snapshot with sequence number snapshot_seq.
  // Returns true if commit_seq <= snapshot_seq
  // If the snapshot_seq is already released and snapshot_seq <= max, sets
  // *snap_released to true and returns true as well.
  inline bool IsInSnapshot(uint64_t prep_seq, uint64_t snapshot_seq,
                           uint64_t min_uncommitted = kMinUnCommittedSeq,
                           bool* snap_released = nullptr) const {
    ROCKS_LOG_DETAILS(info_log_,
                      "IsInSnapshot %" PRIu64 " in %" PRIu64
                      " min_uncommitted %" PRIu64,
                      prep_seq, snapshot_seq, min_uncommitted);
    assert(min_uncommitted >= kMinUnCommittedSeq);
    // Caller is responsible to initialize snap_released.
    assert(snap_released == nullptr || *snap_released == false);
    // Here we try to infer the return value without looking into prepare list.
    // This would help avoiding synchronization over a shared map.
    // TODO(myabandeh): optimize this. This sequence of checks must be correct
    // but not necessary efficient
    if (prep_seq == 0) {
      // Compaction will output keys to bottom-level with sequence number 0 if
      // it is visible to the earliest snapshot.
      ROCKS_LOG_DETAILS(
          info_log_, "IsInSnapshot %" PRIu64 " in %" PRIu64 " returns %" PRId32,
          prep_seq, snapshot_seq, 1);
      return true;
    }

    // 快照 < 事务提交seq，表示未来的事务，不可见
    if (snapshot_seq < prep_seq) {
      // snapshot_seq < prep_seq <= commit_seq => snapshot_seq < commit_seq
      ROCKS_LOG_DETAILS(
          info_log_, "IsInSnapshot %" PRIu64 " in %" PRIu64 " returns %" PRId32,
          prep_seq, snapshot_seq, 0);
      return false;
    }

    // min_uncommitted: 最小未提交事务seq(当前快照下的)
    // 事务提交seq < min_uncommitted,表示该事务已提交,可见
    if (prep_seq < min_uncommitted) {
      ROCKS_LOG_DETAILS(info_log_,
                        "IsInSnapshot %" PRIu64 " in %" PRIu64
                        " returns %" PRId32
                        " because of min_uncommitted %" PRIu64,
                        prep_seq, snapshot_seq, 1, min_uncommitted);
      return true;
    }

    // Commit of delayed prepared(保存的是未提交事务) has two non-atomic steps: add to commit cache,
    // remove from delayed prepared.(delayed prepared中事务的提交是有两个非原子操作:添加到commit cache，从delayed prepared中移除) Our reads from these two is also
    // non-atomic. By looking into commit cache first thus we might not find the
    // prep_seq neither in commit cache not in delayed_prepared_.(先观察commit cache的话，可能不会在commit cache和delayed_prepared_中找到prep_seq) To fix that i)
    // we check if there was any delayed prepared BEFORE looking into commit
    // cache(先检查是否存在delayed prepared), ii) if there was, we complete the search steps to be these: i)
    // commit cache, ii) delayed prepared, commit cache again.(2次添加已提交事务到commit cache) In this way if
    // the first query to commit cache missed the commit, the 2nd will catch it.

    // 这里简单理解就是delayed prepared阶段的是事务提交需要两次操作:添加到commit cache和从delayed prepared中移除。
    // 如果先查询commit cache在查询delayed prepared的话，可能会漏掉，这时需要在查询一次commit cache来避免这个情况。
    // FIXME 这里如果是deplayed prepared和commit cache的转换，那delayed_prepared_commits_这个是干什么用的
    bool was_empty;

    // 最大已提交的事务准备序列
    SequenceNumber max_evicted_seq_lb, max_evicted_seq_ub;
    CommitEntry64b dont_care;

    // prep_seq在commit cache中的索引位置
    auto indexed_seq = prep_seq % COMMIT_CACHE_SIZE;
    size_t repeats = 0;

    /*
     * 这里do-while的处理流程如下(循环条件是,1和5读出来值不相等，如果1 != 5，则意味着在读commit cache期间发生evict,新的已提交事务加进来,淘汰事务会更新max_evicted_seq_,prepared_txns_(最小堆)中小于max_evicted_seq_的事务会写到delayed_prepared)
     * 1.读max_evicted_seq_
     * 2.判断delayed_prepared是否未空
     * 3.commit cache取出对应事务seq
     * 4.如果存在且没有被覆盖，则比较commit seq和快照seq确定是否可见
     * 5.再读max_evicted_seq_,这里如果和1的值不相等，直接continue(表示1-5之间有新的事务已被提交,旧事务被淘汰)
     * 6.如果max_evicted_seq_ < prepare_seq，则表示要么存在于commit cache；要么不存在
     * 7.如果delayed_prepared 不为空:
     *      1)读锁
     *      2)如果不存在于delayed_prepared中，则再次执行3、4、5步骤(catch)(这里可能发生delayed_prepared刚好commit,需要再次读取确认) (这里5之后，如果在4-5之间有新事务提交，旧事务被淘汰会更新max_evicted_seq_,则符合do-while循环条件进入下一次循环)
     *      3)如果存在，则再判断是否存在于delayed_prepared_commits中，存在则比较commit seq和快照seq确定是否可见.否则是未提交
     *
     *  max_evicted_seq_发生变动，则需要检查commit cache是否有目标事务提交。
     *  同样在检查完commit cache后读max_evicted_seq_的两步操作同样不是原子，则需要再次确认是否发生变动
     * */
    do {
      repeats++;
      assert(repeats < 100);
      if (UNLIKELY(repeats >= 100)) {
        throw std::runtime_error(
            "The read was intrupted 100 times by update to max_evicted_seq_. "
            "This is unexpected in all setups");
      }

      // 取出最大淘汰的事务准备序列
      max_evicted_seq_lb = max_evicted_seq_.load(std::memory_order_acquire);
      TEST_SYNC_POINT(
          "WritePreparedTxnDB::IsInSnapshot:max_evicted_seq_:pause");
      TEST_SYNC_POINT(
          "WritePreparedTxnDB::IsInSnapshot:max_evicted_seq_:resume");

      // delayed_prepared_是否空标志
      was_empty = delayed_prepared_empty_.load(std::memory_order_acquire);
      TEST_SYNC_POINT(
          "WritePreparedTxnDB::IsInSnapshot:delayed_prepared_empty_:pause");
      TEST_SYNC_POINT(
          "WritePreparedTxnDB::IsInSnapshot:delayed_prepared_empty_:resume");
      CommitEntry cached;

      // 从commit cache中取出indexed_seq对应的commit entry
      bool exist = GetCommitEntry(indexed_seq, &dont_care, &cached);
      TEST_SYNC_POINT("WritePreparedTxnDB::IsInSnapshot:GetCommitEntry:pause");
      TEST_SYNC_POINT("WritePreparedTxnDB::IsInSnapshot:GetCommitEntry:resume");

      // 存在且是本事务，即已提交
      if (exist && prep_seq == cached.prep_seq) {
        // It is committed and also not evicted from commit cache
        ROCKS_LOG_DETAILS(
            info_log_,
            "IsInSnapshot %" PRIu64 " in %" PRIu64 " returns %" PRId32,
            prep_seq, snapshot_seq, cached.commit_seq <= snapshot_seq);
        return cached.commit_seq <= snapshot_seq;
      }
      // else it could be committed but not inserted in the map which could
      // happen after recovery, or it could be committed and evicted by another
      // commit, or never committed.

      // At this point we dont know if it was committed or it is still prepared
      // 注意，这个点又取了一次(max_evicted_seq_是从commit cache淘汰出来的事务，已经提交，但不确定在这个快照下是否是提交状态)
      max_evicted_seq_ub = max_evicted_seq_.load(std::memory_order_acquire);
      if (UNLIKELY(max_evicted_seq_lb != max_evicted_seq_ub)) {  //这里分支预测，几乎不存在不相等的情况
        continue;
      }
      // Note: max_evicted_seq_ when we did GetCommitEntry <= max_evicted_seq_ub
      // 序列比max_evicted_seq_大，表示两种情况，要么存在于commit cache，已提交了(这个上面已经判断过了)；要么不存在，未提交
      if (max_evicted_seq_ub < prep_seq) {
        // Not evicted from cache and also not present, so must be still
        // prepared
        ROCKS_LOG_DETAILS(info_log_,
                          "IsInSnapshot %" PRIu64 " in %" PRIu64
                          " returns %" PRId32,
                          prep_seq, snapshot_seq, 0);
        return false;
      }
      TEST_SYNC_POINT("WritePreparedTxnDB::IsInSnapshot:prepared_mutex_:pause");
      TEST_SYNC_POINT(
          "WritePreparedTxnDB::IsInSnapshot:prepared_mutex_:resume");

      // delayed_prepared_不为空
      if (!was_empty) {
        // We should not normally reach here
        WPRecordTick(TXN_PREPARE_MUTEX_OVERHEAD);

        // 读锁
        ReadLock rl(&prepared_mutex_);
        ROCKS_LOG_WARN(
            info_log_, "prepared_mutex_ overhead %" PRIu64 " for %" PRIu64,
            static_cast<uint64_t>(delayed_prepared_.size()), prep_seq);

        // 存在
        if (delayed_prepared_.find(prep_seq) != delayed_prepared_.end()) {
          // This is the order: 1) delayed_prepared_commits_ update, 2) publish
          // 3) delayed_prepared_ clean up. So check if it is the case of a late
          // cleanup.

          // uncommitted
          auto it = delayed_prepared_commits_.find(prep_seq);
          if (it == delayed_prepared_commits_.end()) {
            // Then it is not committed yet
            ROCKS_LOG_DETAILS(info_log_,
                              "IsInSnapshot %" PRIu64 " in %" PRIu64
                              " returns %" PRId32,
                              prep_seq, snapshot_seq, 0);
            return false;
          } else {
            ROCKS_LOG_DETAILS(info_log_,
                              "IsInSnapshot %" PRIu64 " in %" PRIu64
                              " commit: %" PRIu64 " returns %" PRId32,
                              prep_seq, snapshot_seq, it->second,
                              snapshot_seq <= it->second);
            return it->second <= snapshot_seq; // 确认已提交的事务对快照可见
          }
        } else {
          // 2nd query to commit cache. Refer to was_empty comment above.
          // 再次从commit cache中取出indexed_seq对应的commit entry
          exist = GetCommitEntry(indexed_seq, &dont_care, &cached);

          // 存在
          if (exist && prep_seq == cached.prep_seq) {
            ROCKS_LOG_DETAILS(
                info_log_,
                "IsInSnapshot %" PRIu64 " in %" PRIu64 " returns %" PRId32,
                prep_seq, snapshot_seq, cached.commit_seq <= snapshot_seq);
            return cached.commit_seq <= snapshot_seq;
          }
          max_evicted_seq_ub = max_evicted_seq_.load(std::memory_order_acquire);
        }
      }
    } while (UNLIKELY(max_evicted_seq_lb != max_evicted_seq_ub)); //这里分支预测，几乎不存在不相等的情况
    // When advancing max_evicted_seq_, we move older entires from prepared to
    // delayed_prepared_. Also we move evicted entries from commit cache to
    // old_commit_map_ if it overlaps(重叠) with any snapshot. Since prep_seq <=
    // max_evicted_seq_, we have three cases: i) in delayed_prepared_, ii) in
    // old_commit_map_, iii) committed with no conflict with any snapshot.
    // 当max_evicted_seq_有变动后，将老的准备事务添加到delayed_prepared_中
    // 因为执行到这里,prep_seq <= max_evicted_seq_的状态,有三个场景:
    // delayed_prepared_: 上面已经确认过，不存在
    // old_commit_map_:   当commit cache发生evicted时,对于某个快照不可见的话，会将其存放到这个map中，以key -> list形式存储，key表示对应快照.
    // 已提交对于任意快照可见: 已提交，可见

    // Case (i) delayed_prepared_ is checked above
    if (max_evicted_seq_ub < snapshot_seq) {  // then (ii) cannot be the case
      // only (iii) is the case: committed  //这里如果快照是大于max_evicted_seq_ub的，即Case 2的map存储的事务一定都是可见的，因为被evicted出来的一定都是小于max_evicted_seq_ub且已提交的快照
      // commit_seq <= max_evicted_seq_ < snapshot_seq => commit_seq <
      // snapshot_seq
      ROCKS_LOG_DETAILS(
          info_log_, "IsInSnapshot %" PRIu64 " in %" PRIu64 " returns %" PRId32,
          prep_seq, snapshot_seq, 1);
      return true;
    }
    // else (ii) might be the case: check the commit data saved for this
    // snapshot. If there was no overlapping commit entry, then it is committed
    // with a commit_seq lower than any live snapshot, including snapshot_seq.
    // old_commit_map,保存当前snapshot下的uncommitted的事务列表,为空则一定提交，可见的，见L312的分析
    if (old_commit_map_empty_.load(std::memory_order_acquire)) {
      ROCKS_LOG_DETAILS(info_log_,
                        "IsInSnapshot %" PRIu64 " in %" PRIu64
                        " returns %" PRId32 " released=1",
                        prep_seq, snapshot_seq, 0);
      assert(snap_released);
      // This snapshot is not valid anymore. We cannot tell if prep_seq is
      // committed before or after the snapshot. Return true but also set
      // snap_released to true.
      *snap_released = true;
      return true;
    }

    //针对很老的快照处理
    {
      // We should not normally reach here unless sapshot_seq is old. This is a
      // rare case and it is ok to pay the cost of mutex ReadLock for such old,
      // reading transactions.
      WPRecordTick(TXN_OLD_COMMIT_MAP_MUTEX_OVERHEAD);
      ReadLock rl(&old_commit_map_mutex_);
      auto prep_set_entry = old_commit_map_.find(snapshot_seq);
      bool found = prep_set_entry != old_commit_map_.end();

      // 如果存在某个快照，寻找对应目标的事务seq是否存在
      if (found) {
        auto& vec = prep_set_entry->second;
        found = std::binary_search(vec.begin(), vec.end(), prep_seq);
      } else {
        // coming from compaction
        ROCKS_LOG_DETAILS(info_log_,
                          "IsInSnapshot %" PRIu64 " in %" PRIu64
                          " returns %" PRId32 " released=1",
                          prep_seq, snapshot_seq, 0);

        // snapshot不存在，需要设置标志
        // This snapshot is not valid anymore. We cannot tell if prep_seq is
        // committed before or after the snapshot. Return true but also set
        // snap_released to true.
        assert(snap_released);
        *snap_released = true;
        return true;
      }

      // 不存在于目标快照的list中,则意味着对应的事务已经提交，可见
      if (!found) {
        ROCKS_LOG_DETAILS(info_log_,
                          "IsInSnapshot %" PRIu64 " in %" PRIu64
                          " returns %" PRId32,
                          prep_seq, snapshot_seq, 1);
        return true;
      }
    }
    // (ii) it the case: it is committed but after the snapshot_seq
    // 这里接着L375的逻辑，如果found，意味着事务已提交，但是对于当前快照是未提交，不可见
    ROCKS_LOG_DETAILS(
        info_log_, "IsInSnapshot %" PRIu64 " in %" PRIu64 " returns %" PRId32,
        prep_seq, snapshot_seq, 0);
    return false;
  }

  // Add the transaction with prepare sequence seq to the prepared list.
  // Note: must be called serially with increasing seq on each call.
  // 新事务会先加入到prepared txns中，并尝试是否需要CheckPreparedAgainstMax
  void AddPrepared(uint64_t seq);
  // Check if any of the prepared txns are less than new max_evicted_seq_. Must
  // be called with prepared_mutex_ write locked.
  // 将prepared txns中小于max_evicted_seq_的事务转移到delay prepared中
  void CheckPreparedAgainstMax(SequenceNumber new_max);
  // Remove the transaction with prepare sequence seq from the prepared list
  void RemovePrepared(const uint64_t seq, const size_t batch_cnt = 1);
  // Add the transaction with prepare sequence prepare_seq and commit sequence
  // commit_seq to the commit map. loop_cnt is to detect infinite loops.
  // Note: must be called serially.
  void AddCommitted(uint64_t prepare_seq, uint64_t commit_seq,
                    uint8_t loop_cnt = 0);

  struct CommitEntry {
    uint64_t prep_seq;
    uint64_t commit_seq;
    CommitEntry() : prep_seq(0), commit_seq(0) {}
    CommitEntry(uint64_t ps, uint64_t cs) : prep_seq(ps), commit_seq(cs) {}
    bool operator==(const CommitEntry& rhs) const {
      return prep_seq == rhs.prep_seq && commit_seq == rhs.commit_seq;
    }
  };

  struct CommitEntry64bFormat {
    explicit CommitEntry64bFormat(size_t index_bits)
        : INDEX_BITS(index_bits),
          PREP_BITS(static_cast<size_t>(64 - PAD_BITS - INDEX_BITS)),
          COMMIT_BITS(static_cast<size_t>(64 - PREP_BITS)),
          COMMIT_FILTER(static_cast<uint64_t>((1ull << COMMIT_BITS) - 1)),
          DELTA_UPPERBOUND(static_cast<uint64_t>((1ull << COMMIT_BITS))) {}
    // Number of higher bits of a sequence number that is not used. They are
    // used to encode the value type, ...
    const size_t PAD_BITS = static_cast<size_t>(8);
    // Number of lower bits from prepare seq that can be skipped as they are
    // implied by the index of the entry in the array
    const size_t INDEX_BITS;
    // Number of bits we use to encode the prepare seq
    const size_t PREP_BITS;
    // Number of bits we use to encode the commit seq.
    const size_t COMMIT_BITS;
    // Filter to encode/decode commit seq
    const uint64_t COMMIT_FILTER;
    // The value of commit_seq - prepare_seq + 1 must be less than this bound
    const uint64_t DELTA_UPPERBOUND;
  };

  // Prepare Seq (64 bits) = PAD ... PAD PREP PREP ... PREP INDEX INDEX ...
  // INDEX Delta Seq (64 bits)   = 0 0 0 0 0 0 0 0 0  0 0 0 DELTA DELTA ...
  // DELTA DELTA Encoded Value         = PREP PREP .... PREP PREP DELTA DELTA
  // ... DELTA DELTA PAD: first bits of a seq that is reserved for tagging and
  // hence ignored PREP/INDEX: the used bits in a prepare seq number INDEX: the
  // bits that do not have to be encoded (will be provided externally) DELTA:
  // prep seq - commit seq + 1 Number of DELTA bits should be equal to number of
  // index bits + PADs
  struct CommitEntry64b {
    constexpr CommitEntry64b() noexcept : rep_(0) {}

    CommitEntry64b(const CommitEntry& entry, const CommitEntry64bFormat& format)
        : CommitEntry64b(entry.prep_seq, entry.commit_seq, format) {}

    CommitEntry64b(const uint64_t ps, const uint64_t cs,
                   const CommitEntry64bFormat& format) {
      assert(ps < static_cast<uint64_t>(
                      (1ull << (format.PREP_BITS + format.INDEX_BITS))));
      assert(ps <= cs);
      uint64_t delta = cs - ps + 1;  // make initialized delta always >= 1
      // zero is reserved for uninitialized entries
      assert(0 < delta);
      assert(delta < format.DELTA_UPPERBOUND);
      if (delta >= format.DELTA_UPPERBOUND) {
        throw std::runtime_error(
            "commit_seq >> prepare_seq. The allowed distance is " +
            ToString(format.DELTA_UPPERBOUND) + " commit_seq is " +
            ToString(cs) + " prepare_seq is " + ToString(ps));
      }
      rep_ = (ps << format.PAD_BITS) & ~format.COMMIT_FILTER;
      rep_ = rep_ | delta;
    }

    // Return false if the entry is empty
    bool Parse(const uint64_t indexed_seq, CommitEntry* entry,
               const CommitEntry64bFormat& format) {
      uint64_t delta = rep_ & format.COMMIT_FILTER;
      // zero is reserved for uninitialized entries
      assert(delta < static_cast<uint64_t>((1ull << format.COMMIT_BITS)));
      if (delta == 0) {
        return false;  // initialized entry would have non-zero delta
      }

      assert(indexed_seq < static_cast<uint64_t>((1ull << format.INDEX_BITS)));
      uint64_t prep_up = rep_ & ~format.COMMIT_FILTER;
      prep_up >>= format.PAD_BITS;
      const uint64_t& prep_low = indexed_seq;
      entry->prep_seq = prep_up | prep_low;

      entry->commit_seq = entry->prep_seq + delta - 1;
      return true;
    }

   private:
    uint64_t rep_;
  };

  // Struct to hold ownership of snapshot and read callback for cleanup.
  struct IteratorState;

  std::shared_ptr<std::map<uint32_t, const Comparator*>> GetCFComparatorMap() {
    return cf_map_;
  }
  std::shared_ptr<std::map<uint32_t, ColumnFamilyHandle*>> GetCFHandleMap() {
    return handle_map_;
  }
  void UpdateCFComparatorMap(
      const std::vector<ColumnFamilyHandle*>& handles) override;
  void UpdateCFComparatorMap(ColumnFamilyHandle* handle) override;

  virtual const Snapshot* GetSnapshot() override;
  SnapshotImpl* GetSnapshotInternal(bool for_ww_conflict_check);

 protected:
  virtual Status VerifyCFOptions(
      const ColumnFamilyOptions& cf_options) override;

 private:
  friend class PreparedHeap_BasicsTest_Test;
  friend class PreparedHeap_Concurrent_Test;
  friend class PreparedHeap_EmptyAtTheEnd_Test;
  friend class SnapshotConcurrentAccessTest_SnapshotConcurrentAccessTest_Test;
  friend class WritePreparedCommitEntryPreReleaseCallback;
  friend class WritePreparedTransactionTestBase;
  friend class WritePreparedTxn;
  friend class WritePreparedTxnDBMock;
  friend class WritePreparedTransactionTest_AddPreparedBeforeMax_Test;
  friend class WritePreparedTransactionTest_AdvanceMaxEvictedSeqBasicTest_Test;
  friend class
      WritePreparedTransactionTest_AdvanceMaxEvictedSeqWithDuplicatesTest_Test;
  friend class WritePreparedTransactionTest_AdvanceSeqByOne_Test;
  friend class WritePreparedTransactionTest_BasicRecoveryTest_Test;
  friend class WritePreparedTransactionTest_CheckAgainstSnapshotsTest_Test;
  friend class WritePreparedTransactionTest_CleanupSnapshotEqualToMax_Test;
  friend class
      WritePreparedTransactionTest_ConflictDetectionAfterRecoveryTest_Test;
  friend class WritePreparedTransactionTest_CommitMapTest_Test;
  friend class WritePreparedTransactionTest_DoubleSnapshot_Test;
  friend class WritePreparedTransactionTest_IsInSnapshotEmptyMapTest_Test;
  friend class WritePreparedTransactionTest_IsInSnapshotReleased_Test;
  friend class WritePreparedTransactionTest_IsInSnapshotTest_Test;
  friend class WritePreparedTransactionTest_NewSnapshotLargerThanMax_Test;
  friend class WritePreparedTransactionTest_MaxCatchupWithNewSnapshot_Test;
  friend class
      WritePreparedTransactionTest_NonAtomicCommitOfDelayedPrepared_Test;
  friend class
      WritePreparedTransactionTest_NonAtomicUpdateOfDelayedPrepared_Test;
  friend class WritePreparedTransactionTest_NonAtomicUpdateOfMaxEvictedSeq_Test;
  friend class WritePreparedTransactionTest_OldCommitMapGC_Test;
  friend class WritePreparedTransactionTest_RollbackTest_Test;
  friend class WriteUnpreparedTxnDB;
  friend class WriteUnpreparedTransactionTest_RecoveryTest_Test;

  void Init(const TransactionDBOptions& /* unused */);

  void WPRecordTick(uint32_t ticker_type) const {
    RecordTick(db_impl_->immutable_db_options_.statistics.get(), ticker_type);
  }

  // A heap with the amortized O(1) complexity for erase. It uses one extra heap
  // to keep track of erased entries that are not yet on top of the main heap.
  // 为了方便删除O(1)，这里用了额外一个堆来维护，等到erased_heap_.top的值小于等于heap_时则直接删除
  class PreparedHeap {
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>
        heap_;
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>>
        erased_heap_;
    // True when testing crash recovery
    bool TEST_CRASH_ = false;
    friend class WritePreparedTxnDB;

   public:
    ~PreparedHeap() {
      if (!TEST_CRASH_) {
        assert(heap_.empty());
        assert(erased_heap_.empty());
      }
    }
    bool empty() { return heap_.empty(); }
    uint64_t top() { return heap_.top(); }
    void push(uint64_t v) { heap_.push(v); }
    void pop() {
      heap_.pop();
      while (!heap_.empty() && !erased_heap_.empty() &&
             // heap_.top() > erased_heap_.top() could happen if we have erased
             // a non-existent entry. Ideally the user should not do that but we
             // should be resilient against it.
             heap_.top() >= erased_heap_.top()) {
        if (heap_.top() == erased_heap_.top()) {
          heap_.pop();
        }
        uint64_t erased __attribute__((__unused__));
        erased = erased_heap_.top();
        erased_heap_.pop();
        // No duplicate prepare sequence numbers
        assert(erased_heap_.empty() || erased_heap_.top() != erased);
      }
      while (heap_.empty() && !erased_heap_.empty()) {
        erased_heap_.pop();
      }
    }
    void erase(uint64_t seq) {
      if (!heap_.empty()) {
        if (seq < heap_.top()) {
          // Already popped, ignore it.
        } else if (heap_.top() == seq) {
          pop();
          assert(heap_.empty() || heap_.top() != seq);
        } else {  // (heap_.top() > seq)
          // Down the heap, remember to pop it later
          erased_heap_.push(seq);
        }
      }
    }
  };

  void TEST_Crash() override { prepared_txns_.TEST_CRASH_ = true; }

  // Get the commit entry with index indexed_seq from the commit table. It
  // returns true if such entry exists.
  bool GetCommitEntry(const uint64_t indexed_seq, CommitEntry64b* entry_64b,
                      CommitEntry* entry) const;

  // Rewrite the entry with the index indexed_seq in the commit table with the
  // commit entry <prep_seq, commit_seq>. If the rewrite results into eviction,
  // sets the evicted_entry and returns true.
  bool AddCommitEntry(const uint64_t indexed_seq, const CommitEntry& new_entry,
                      CommitEntry* evicted_entry);

  // Rewrite the entry with the index indexed_seq in the commit table with the
  // commit entry new_entry only if the existing entry matches the
  // expected_entry. Returns false otherwise.
  bool ExchangeCommitEntry(const uint64_t indexed_seq,
                           CommitEntry64b& expected_entry,
                           const CommitEntry& new_entry);

  // Increase max_evicted_seq_ from the previous value prev_max to the new
  // value. This also involves taking care of prepared txns that are not
  // committed before new_max, as well as updating the list of live snapshots at
  // the time of updating the max. Thread-safety: this function can be called
  // concurrently. The concurrent invocations of this function is equivalent to
  // a serial invocation in which the last invocation is the one with the
  // largest new_max value.

  // 更新max_evicted_seq_到更大的值，同时更新prepared txns和old_map_commit
  void AdvanceMaxEvictedSeq(const SequenceNumber& prev_max,
                            const SequenceNumber& new_max);

  inline SequenceNumber SmallestUnCommittedSeq() {
    // Since we update the prepare_heap always from the main write queue via
    // PreReleaseCallback, the prepared_txns_.top() indicates the smallest
    // prepared data in 2pc transactions. For non-2pc transactions that are
    // written in two steps, we also update prepared_txns_ at the first step
    // (via the same mechanism) so that their uncommitted data is reflected in
    // SmallestUnCommittedSeq.
    ReadLock rl(&prepared_mutex_);
    // Since we are holding the mutex, and GetLatestSequenceNumber is updated
    // after prepared_txns_ are, the value of GetLatestSequenceNumber would
    // reflect any uncommitted data that is not added to prepared_txns_ yet.
    // Otherwise, if there is no concurrent txn, this value simply reflects that
    // latest value in the memtable.
    if (!delayed_prepared_.empty()) {
      assert(!delayed_prepared_empty_.load());
      return *delayed_prepared_.begin();
    }
    if (prepared_txns_.empty()) {
      return db_impl_->GetLatestSequenceNumber() + 1;
    } else {
      return std::min(prepared_txns_.top(),
                      db_impl_->GetLatestSequenceNumber() + 1);
    }
  }
  // Enhance the snapshot object by recording in it the smallest uncommitted seq
  inline void EnhanceSnapshot(SnapshotImpl* snapshot,
                              SequenceNumber min_uncommitted) {
    assert(snapshot);
    snapshot->min_uncommitted_ = min_uncommitted;
  }

  virtual const std::vector<SequenceNumber> GetSnapshotListFromDB(
      SequenceNumber max);

  // Will be called by the public ReleaseSnapshot method. Does the maintenance
  // internal to WritePreparedTxnDB
  void ReleaseSnapshotInternal(const SequenceNumber snap_seq);

  // Update the list of snapshots corresponding to the soon-to-be-updated
  // max_evicted_seq_. Thread-safety: this function can be called concurrently.
  // The concurrent invocations of this function is equivalent to a serial
  // invocation in which the last invocation is the one with the largest
  // version value.
  void UpdateSnapshots(const std::vector<SequenceNumber>& snapshots,
                       const SequenceNumber& version);
  // Check the new list of new snapshots against the old one to see  if any of
  // the snapshots are released and to do the cleanup for the released snapshot.
  void CleanupReleasedSnapshots(
      const std::vector<SequenceNumber>& new_snapshots,
      const std::vector<SequenceNumber>& old_snapshots);

  // Check an evicted entry against live snapshots to see if it should be kept
  // around or it can be safely discarded (and hence assume committed for all
  // snapshots). Thread-safety: this function can be called concurrently. If it
  // is called concurrently with multiple UpdateSnapshots, the result is the
  // same as checking the intersection of the snapshot list before updates with
  // the snapshot list of all the concurrent updates.
  void CheckAgainstSnapshots(const CommitEntry& evicted);

  // Add a new entry to old_commit_map_ if prep_seq <= snapshot_seq <
  // commit_seq. Return false if checking the next snapshot(s) is not needed.
  // This is the case if none of the next snapshots could satisfy the condition.
  // next_is_larger: the next snapshot will be a larger value

  // 这里调用的时机在新的事务被提交后，考虑到对于live snapshot的应用，针对与之重叠的事务需要记录到old_commit_map_中
  bool MaybeUpdateOldCommitMap(const uint64_t& prep_seq,
                               const uint64_t& commit_seq,
                               const uint64_t& snapshot_seq,
                               const bool next_is_larger);

  // A trick to increase the last visible sequence number by one and also wait
  // for the in-flight commits to be visible.
  void AdvanceSeqByOne();

  // The list of live snapshots at the last time that max_evicted_seq_ advanced.
  // The list stored into two data structures: in snapshot_cache_ that is
  // efficient for concurrent reads, and in snapshots_ if the data does not fit
  // into snapshot_cache_. The total number of snapshots in the two lists
  std::atomic<size_t> snapshots_total_ = {};
  // The list sorted in ascending order. Thread-safety for writes is provided
  // with snapshots_mutex_ and concurrent reads are safe due to std::atomic for
  // each entry. In x86_64 architecture such reads are compiled to simple read
  // instructions.
  const size_t SNAPSHOT_CACHE_BITS;
  const size_t SNAPSHOT_CACHE_SIZE;
  std::unique_ptr<std::atomic<SequenceNumber>[]> snapshot_cache_;
  // 2nd list for storing snapshots. The list sorted in ascending order.
  // Thread-safety is provided with snapshots_mutex_.
  std::vector<SequenceNumber> snapshots_;
  // The list of all snapshots: snapshots_ + snapshot_cache_. This list although
  // redundant but simplifies CleanupOldSnapshots implementation.
  // Thread-safety is provided with snapshots_mutex_.
  std::vector<SequenceNumber> snapshots_all_;
  // The version of the latest list of snapshots. This can be used to avoid
  // rewriting a list that is concurrently updated with a more recent version.
  SequenceNumber snapshots_version_ = 0;

  // A heap of prepared transactions. Thread-safety is provided with
  // prepared_mutex_.
  // 新准备事物序列先加入到这里
  PreparedHeap prepared_txns_;
  const size_t COMMIT_CACHE_BITS;
  const size_t COMMIT_CACHE_SIZE;
  const CommitEntry64bFormat FORMAT;
  // commit_cache_ must be initialized to zero to tell apart an empty index from
  // a filled one. Thread-safety is provided with commit_cache_mutex_.
  std::unique_ptr<std::atomic<CommitEntry64b>[]> commit_cache_;
  // The largest evicted *commit* sequence number from the commit_cache_. If a
  // seq is smaller than max_evicted_seq_ is might or might not be present in
  // commit_cache_. So commit_cache_ must first be checked before consulting
  // with(查看) max_evicted_seq_.
  std::atomic<uint64_t> max_evicted_seq_ = {};
  // Order: 1) update future_max_evicted_seq_ = new_max, 2)
  // GetSnapshotListFromDB(new_max), max_evicted_seq_ = new_max. Since
  // GetSnapshotInternal guarantess(保证) that the snapshot seq is larger than
  // future_max_evicted_seq_, this guarantes that if a snapshot is not larger
  // than max has already being looked at via a GetSnapshotListFromDB(new_max).
  // 既然是担保人(保证) 快照seq大于B，这保证了如果快照不大于max，则已通过C查看。
  std::atomic<uint64_t> future_max_evicted_seq_ = {};
  // Advance max_evicted_seq_ by this value each time it needs an update. The
  // larger the value, the less frequent advances we would have. We do not want
  // it to be too large either as it would cause stalls by doing too much
  // maintenance work under the lock.
  size_t INC_STEP_FOR_MAX_EVICTED = 1;
  // A map from old snapshots (expected to be used by a few read-only txns) to
  // prepared sequence number of the evicted entries from commit_cache_ that
  // overlaps with such snapshot. These are the prepared sequence numbers that
  // the snapshot, to which they are mapped, cannot assume to be committed just
  // because it is no longer in the commit_cache_. The vector must be sorted
  // after each update.
  // Thread-safety is provided with old_commit_map_mutex_.
  std::map<SequenceNumber, std::vector<SequenceNumber>> old_commit_map_; //保存的是对于某个快照来说不可见的事务

  // 记录在max_evicted_seq_之前的prepared的事务序列
  // A set of long-running prepared transactions that are not finished by the
  // time max_evicted_seq_ advances their sequence number. This is expected to
  // be empty normally. Thread-safety is provided with prepared_mutex_.
  std::set<uint64_t> delayed_prepared_;

  // 这个用来区分unprepared的事务和已经提交没有清理的事务,个人理解delayed_prepared_中后来commit的事务会加到commit cache中,并更新delayed_prepared_commits_
  // 这个缓存可能会定期清理(FIXME 需要确认下,可参考L268)
  // Commit of a delayed prepared: 1) update commit cache, 2) update
  // delayed_prepared_commits_, 3) publish seq, 3) clean up delayed_prepared_.
  // delayed_prepared_commits_ will help us tell apart the unprepared txns from
  // the ones that are committed but not cleaned up yet.
  std::unordered_map<SequenceNumber, SequenceNumber> delayed_prepared_commits_;
  // Update when delayed_prepared_.empty() changes. Expected to be true
  // normally.
  std::atomic<bool> delayed_prepared_empty_ = {true};
  // Update when old_commit_map_.empty() changes. Expected to be true normally.
  std::atomic<bool> old_commit_map_empty_ = {true};
  mutable port::RWMutex prepared_mutex_;
  mutable port::RWMutex old_commit_map_mutex_;
  mutable port::RWMutex commit_cache_mutex_;
  mutable port::RWMutex snapshots_mutex_;
  // A cache of the cf comparators
  // Thread safety: since it is a const it is safe to read it concurrently
  std::shared_ptr<std::map<uint32_t, const Comparator*>> cf_map_;
  // A cache of the cf handles
  // Thread safety: since the handle is read-only object it is a const it is
  // safe to read it concurrently
  std::shared_ptr<std::map<uint32_t, ColumnFamilyHandle*>> handle_map_;
};

class WritePreparedTxnReadCallback : public ReadCallback {
 public:
  WritePreparedTxnReadCallback(WritePreparedTxnDB* db, SequenceNumber snapshot)
      : ReadCallback(snapshot), db_(db) {}
  WritePreparedTxnReadCallback(WritePreparedTxnDB* db, SequenceNumber snapshot,
                               SequenceNumber min_uncommitted)
      : ReadCallback(snapshot, min_uncommitted), db_(db) {}

  // Will be called to see if the seq number visible; if not it moves on to
  // the next seq number.
  inline virtual bool IsVisibleFullCheck(SequenceNumber seq) override {
    auto snapshot = max_visible_seq_;
    return db_->IsInSnapshot(seq, snapshot, min_uncommitted_);
  }

  // TODO(myabandeh): override Refresh when Iterator::Refresh is supported
 private:
  WritePreparedTxnDB* db_;
};

class AddPreparedCallback : public PreReleaseCallback {
 public:
  AddPreparedCallback(WritePreparedTxnDB* db, DBImpl* db_impl,
                      size_t sub_batch_cnt, bool two_write_queues,
                      bool first_prepare_batch)
      : db_(db),
        db_impl_(db_impl),
        sub_batch_cnt_(sub_batch_cnt),
        two_write_queues_(two_write_queues),
        first_prepare_batch_(first_prepare_batch) {
    (void)two_write_queues_;  // to silence unused private field warning
  }
  virtual Status Callback(SequenceNumber prepare_seq,
                          bool is_mem_disabled __attribute__((__unused__)),
                          uint64_t log_number) override {
    // Always Prepare from the main queue
    assert(!two_write_queues_ || !is_mem_disabled);  // implies the 1st queue
    for (size_t i = 0; i < sub_batch_cnt_; i++) {
      db_->AddPrepared(prepare_seq + i);
    }
    if (first_prepare_batch_) {
      assert(log_number != 0);
      db_impl_->logs_with_prep_tracker()->MarkLogAsContainingPrepSection(
          log_number);
    }
    return Status::OK();
  }

 private:
  WritePreparedTxnDB* db_;
  DBImpl* db_impl_;
  size_t sub_batch_cnt_;
  bool two_write_queues_;
  // It is 2PC and this is the first prepare batch. Always the case in 2PC
  // unless it is WriteUnPrepared.
  bool first_prepare_batch_;
};

class WritePreparedCommitEntryPreReleaseCallback : public PreReleaseCallback {
 public:
  // includes_data indicates that the commit also writes non-empty
  // CommitTimeWriteBatch to memtable, which needs to be committed separately.
  WritePreparedCommitEntryPreReleaseCallback(
      WritePreparedTxnDB* db, DBImpl* db_impl, SequenceNumber prep_seq,
      size_t prep_batch_cnt, size_t data_batch_cnt = 0,
      SequenceNumber aux_seq = kMaxSequenceNumber, size_t aux_batch_cnt = 0)
      : db_(db),
        db_impl_(db_impl),
        prep_seq_(prep_seq),
        prep_batch_cnt_(prep_batch_cnt),
        data_batch_cnt_(data_batch_cnt),
        includes_data_(data_batch_cnt_ > 0),
        aux_seq_(aux_seq),
        aux_batch_cnt_(aux_batch_cnt),
        includes_aux_batch_(aux_batch_cnt > 0) {
    assert((prep_batch_cnt_ > 0) != (prep_seq == kMaxSequenceNumber));  // xor
    assert(prep_batch_cnt_ > 0 || data_batch_cnt_ > 0);
    assert((aux_batch_cnt_ > 0) != (aux_seq == kMaxSequenceNumber));  // xor
  }

  virtual Status Callback(SequenceNumber commit_seq,
                          bool is_mem_disabled __attribute__((__unused__)),
                          uint64_t) override {
    // Always commit from the 2nd queue
    assert(!db_impl_->immutable_db_options().two_write_queues ||
           is_mem_disabled);
    assert(includes_data_ || prep_seq_ != kMaxSequenceNumber);
    // Data batch is what accompanied with the commit marker and affects the
    // last seq in the commit batch.
    const uint64_t last_commit_seq = LIKELY(data_batch_cnt_ <= 1)
                                         ? commit_seq
                                         : commit_seq + data_batch_cnt_ - 1;
    if (prep_seq_ != kMaxSequenceNumber) {
      for (size_t i = 0; i < prep_batch_cnt_; i++) {
        db_->AddCommitted(prep_seq_ + i, last_commit_seq);
      }
    }  // else there was no prepare phase
    if (includes_aux_batch_) {
      for (size_t i = 0; i < aux_batch_cnt_; i++) {
        db_->AddCommitted(aux_seq_ + i, last_commit_seq);
      }
    }
    if (includes_data_) {
      assert(data_batch_cnt_);
      // Commit the data that is accompanied with the commit request
      for (size_t i = 0; i < data_batch_cnt_; i++) {
        // For commit seq of each batch use the commit seq of the last batch.
        // This would make debugging easier by having all the batches having
        // the same sequence number.
        db_->AddCommitted(commit_seq + i, last_commit_seq);
      }
    }
    if (db_impl_->immutable_db_options().two_write_queues) {
      assert(is_mem_disabled);  // implies the 2nd queue
      // Publish the sequence number. We can do that here assuming the callback
      // is invoked only from one write queue, which would guarantee that the
      // publish sequence numbers will be in order, i.e., once a seq is
      // published all the seq prior to that are also publishable.
      db_impl_->SetLastPublishedSequence(last_commit_seq);
    }
    // else SequenceNumber that is updated as part of the write already does the
    // publishing
    return Status::OK();
  }

 private:
  WritePreparedTxnDB* db_;
  DBImpl* db_impl_;
  // kMaxSequenceNumber if there was no prepare phase
  SequenceNumber prep_seq_;
  size_t prep_batch_cnt_;
  size_t data_batch_cnt_;
  // Data here is the batch that is written with the commit marker, either
  // because it is commit without prepare or commit has a CommitTimeWriteBatch.
  bool includes_data_;
  // Auxiliary batch (if there is any) is a batch that is written before, but
  // gets the same commit seq as prepare batch or data batch. This is used in
  // two write queues where the CommitTimeWriteBatch becomes the aux batch and
  // we do a separate write to actually commit everything.
  SequenceNumber aux_seq_;
  size_t aux_batch_cnt_;
  bool includes_aux_batch_;
};

// For two_write_queues commit both the aborted batch and the cleanup batch and
// then published the seq
class WritePreparedRollbackPreReleaseCallback : public PreReleaseCallback {
 public:
  WritePreparedRollbackPreReleaseCallback(WritePreparedTxnDB* db,
                                          DBImpl* db_impl,
                                          SequenceNumber prep_seq,
                                          SequenceNumber rollback_seq,
                                          size_t prep_batch_cnt)
      : db_(db),
        db_impl_(db_impl),
        prep_seq_(prep_seq),
        rollback_seq_(rollback_seq),
        prep_batch_cnt_(prep_batch_cnt) {
    assert(prep_seq != kMaxSequenceNumber);
    assert(rollback_seq != kMaxSequenceNumber);
    assert(prep_batch_cnt_ > 0);
  }

  Status Callback(SequenceNumber commit_seq, bool is_mem_disabled,
                  uint64_t) override {
    // Always commit from the 2nd queue
    assert(is_mem_disabled);  // implies the 2nd queue
    assert(db_impl_->immutable_db_options().two_write_queues);
#ifdef NDEBUG
    (void)is_mem_disabled;
#endif
    const uint64_t last_commit_seq = commit_seq;
    db_->AddCommitted(rollback_seq_, last_commit_seq);
    for (size_t i = 0; i < prep_batch_cnt_; i++) {
      db_->AddCommitted(prep_seq_ + i, last_commit_seq);
    }
    db_impl_->SetLastPublishedSequence(last_commit_seq);
    return Status::OK();
  }

 private:
  WritePreparedTxnDB* db_;
  DBImpl* db_impl_;
  SequenceNumber prep_seq_;
  SequenceNumber rollback_seq_;
  size_t prep_batch_cnt_;
};

// Count the number of sub-batches inside a batch. A sub-batch does not have
// duplicate keys.
struct SubBatchCounter : public WriteBatch::Handler {
  explicit SubBatchCounter(std::map<uint32_t, const Comparator*>& comparators)
      : comparators_(comparators), batches_(1) {}
  std::map<uint32_t, const Comparator*>& comparators_;
  using CFKeys = std::set<Slice, SetComparator>;
  std::map<uint32_t, CFKeys> keys_;
  size_t batches_;
  size_t BatchCount() { return batches_; }
  void AddKey(const uint32_t cf, const Slice& key);
  void InitWithComp(const uint32_t cf);
  Status MarkNoop(bool) override { return Status::OK(); }
  Status MarkEndPrepare(const Slice&) override { return Status::OK(); }
  Status MarkCommit(const Slice&) override { return Status::OK(); }
  Status PutCF(uint32_t cf, const Slice& key, const Slice&) override {
    AddKey(cf, key);
    return Status::OK();
  }
  Status DeleteCF(uint32_t cf, const Slice& key) override {
    AddKey(cf, key);
    return Status::OK();
  }
  Status SingleDeleteCF(uint32_t cf, const Slice& key) override {
    AddKey(cf, key);
    return Status::OK();
  }
  Status MergeCF(uint32_t cf, const Slice& key, const Slice&) override {
    AddKey(cf, key);
    return Status::OK();
  }
  Status MarkBeginPrepare(bool) override { return Status::OK(); }
  Status MarkRollback(const Slice&) override { return Status::OK(); }
  bool WriteAfterCommit() const override { return false; }
};

}  //  namespace rocksdb
#endif  // ROCKSDB_LITE
