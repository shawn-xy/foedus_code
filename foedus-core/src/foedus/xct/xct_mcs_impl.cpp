/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#include "foedus/xct/xct_mcs_impl.hpp"

#include <glog/logging.h>

#include <atomic>

#include "foedus/assert_nd.hpp"
#include "foedus/assorted/atomic_fences.hpp"
#include "foedus/thread/thread_pimpl.hpp"  // just for explicit instantiation at the end
#include "foedus/xct/xct_id.hpp"
#include "foedus/xct/xct_mcs_adapter_impl.hpp"

namespace foedus {
namespace xct {

inline void assert_mcs_aligned(const void* address) {
  ASSERT_ND(address);
  ASSERT_ND(reinterpret_cast<uintptr_t>(address) % 4 == 0);
}

/**
 * Spin locally until the given condition returns true
 * @attention We initially had this method behaving like spin_while, which is opposite!
 * Note that this waits \b UNTIL the condition becomes true@
 */
template <typename COND>
void spin_until(COND spin_until_cond) {
  DVLOG(1) << "Locally spinning...";
  uint64_t spins = 0;
  while (!spin_until_cond()) {
    ++spins;
    if ((spins & 0xFFFFFFU) == 0) {
      assorted::spinlock_yield();
    }
  }
  DVLOG(1) << "Spin ended. Spent " << spins << " spins";
}

////////////////////////////////////////////////////////////////////////////////
///
///      WW-lock implementations (all simple versions)
///  These do not depend on RW_BLOCK, so they are primary templates without
///  partial specialization. So we don't need any trick.
///
////////////////////////////////////////////////////////////////////////////////
template <typename ADAPTOR>
McsBlockIndex McsWwImpl<ADAPTOR>::acquire_unconditional(McsLock* mcs_lock) {
  // Basically _all_ writes in this function must come with some memory barrier. Be careful!
  // Also, the performance of this method really matters, especially that of common path.
  // Check objdump -d. Everything in common path should be inlined.
  // Also, check minimal sufficient mfences (note, xchg implies lock prefix. not a compiler's bug!).
  ASSERT_ND(!adaptor_.me_waiting()->load());
  assert_mcs_aligned(mcs_lock);
  // so far we allow only 2^16 MCS blocks per transaction. we might increase later.
  ASSERT_ND(adaptor_.get_cur_block() < 0xFFFFU);
  McsBlockIndex block_index = adaptor_.issue_new_block();
  ASSERT_ND(block_index > 0);
  ASSERT_ND(block_index <= 0xFFFFU);
  McsBlock* my_block = adaptor_.get_ww_my_block(block_index);
  my_block->clear_successor_release();
  adaptor_.me_waiting()->store(true, std::memory_order_release);
  const thread::ThreadId id = adaptor_.get_my_id();
  uint32_t desired = McsLock::to_int(id, block_index);
  uint32_t group_tail = desired;
  uint32_t* address = &(mcs_lock->data_);
  assert_mcs_aligned(address);

  uint32_t pred_int = 0;
  while (true) {
    // if it's obviously locked by a guest, we should wait until it's released.
    // so far this is busy-wait, we can do sth. to prevent priority inversion later.
    if (UNLIKELY(*address == kMcsGuestId)) {
      spin_until([address]{
        return assorted::atomic_load_acquire<uint32_t>(address) != kMcsGuestId;
      });
    }

    // atomic op should imply full barrier, but make sure announcing the initialized new block.
    ASSERT_ND(group_tail != kMcsGuestId);
    ASSERT_ND(group_tail != 0);
    ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != group_tail);
    pred_int = assorted::raw_atomic_exchange<uint32_t>(address, group_tail);
    ASSERT_ND(pred_int != group_tail);
    ASSERT_ND(pred_int != desired);

    if (pred_int == 0) {
      // this means it was not locked.
      ASSERT_ND(mcs_lock->is_locked());
      DVLOG(2) << "Okay, got a lock uncontended. me=" << id;
      adaptor_.me_waiting()->store(false, std::memory_order_release);
      ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != 0);
      return block_index;
    } else if (UNLIKELY(pred_int == kMcsGuestId)) {
      // ouch, I don't want to keep the guest ID! return it back.
      // This also determines the group_tail of this queue
      group_tail = assorted::raw_atomic_exchange<uint32_t>(address, kMcsGuestId);
      ASSERT_ND(group_tail != 0 && group_tail != kMcsGuestId);
      continue;
    } else {
      break;
    }
  }

  ASSERT_ND(pred_int != 0 && pred_int != kMcsGuestId);
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != 0);
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != kMcsGuestId);
  McsLock old;
  old.data_ = pred_int;
  ASSERT_ND(mcs_lock->is_locked());
  thread::ThreadId predecessor_id = old.get_tail_waiter();
  ASSERT_ND(predecessor_id != id);
  McsBlockIndex predecessor_block = old.get_tail_waiter_block();
  DVLOG(0) << "mm, contended, we have to wait.. me=" << id << " pred=" << predecessor_id;

  ASSERT_ND(adaptor_.me_waiting()->load());
  ASSERT_ND(adaptor_.get_other_cur_block(predecessor_id) >= predecessor_block);
  McsBlock* pred_block = adaptor_.get_ww_other_block(predecessor_id, predecessor_block);
  ASSERT_ND(!pred_block->has_successor());

  pred_block->set_successor_release(id, block_index);

  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != 0);
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != kMcsGuestId);
  spin_until([this]{ return !this->adaptor_.me_waiting()->load(std::memory_order_acquire); });
  DVLOG(1) << "Okay, now I hold the lock. me=" << id << ", ex-pred=" << predecessor_id;
  ASSERT_ND(!adaptor_.me_waiting()->load());
  ASSERT_ND(mcs_lock->is_locked());
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != 0);
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != kMcsGuestId);
  return block_index;
}

template <typename ADAPTOR>
void McsWwImpl<ADAPTOR>::ownerless_acquire_unconditional(McsLock* mcs_lock) {
  // Basically _all_ writes in this function must come with some memory barrier. Be careful!
  // Also, the performance of this method really matters, especially that of common path.
  // Check objdump -d. Everything in common path should be inlined.
  // Also, check minimal sufficient mfences (note, xchg implies lock prefix. not a compiler's bug!).
  assert_mcs_aligned(mcs_lock);
  uint32_t* address = &(mcs_lock->data_);
  assert_mcs_aligned(address);
  spin_until([mcs_lock, address]{
    uint32_t old_int = McsLock::to_int(0, 0);
    return assorted::raw_atomic_compare_exchange_weak<uint32_t>(
      address,
      &old_int,
      kMcsGuestId);
  });
  DVLOG(1) << "Okay, now I hold the lock. me=guest";
  ASSERT_ND(mcs_lock->is_locked());
}

template <typename ADAPTOR>
McsBlockIndex McsWwImpl<ADAPTOR>::initial(McsLock* mcs_lock) {
  // Basically _all_ writes in this function must come with release barrier.
  // This method itself doesn't need barriers, but then we need to later take a seq_cst barrier
  // in an appropriate place. That's hard to debug, so just take release barriers here.
  // Also, everything should be inlined.
  assert_mcs_aligned(mcs_lock);
  ASSERT_ND(!adaptor_.me_waiting()->load());
  ASSERT_ND(!mcs_lock->is_locked());
  // so far we allow only 2^16 MCS blocks per transaction. we might increase later.
  ASSERT_ND(adaptor_.get_cur_block() < 0xFFFFU);

  McsBlockIndex block_index = adaptor_.issue_new_block();
  ASSERT_ND(block_index > 0 && block_index <= 0xFFFFU);
  McsBlock* my_block = adaptor_.get_ww_my_block(block_index);
  my_block->clear_successor_release();
  const thread::ThreadId id = adaptor_.get_my_id();
  mcs_lock->reset_release(id, block_index);
  return block_index;
}

template <typename ADAPTOR>
void McsWwImpl<ADAPTOR>::ownerless_initial(McsLock* mcs_lock) {
  assert_mcs_aligned(mcs_lock);
  ASSERT_ND(!mcs_lock->is_locked());
  mcs_lock->reset_guest_id_release();
}

template <typename ADAPTOR>
void McsWwImpl<ADAPTOR>::release(McsLock* mcs_lock, McsBlockIndex block_index) {
  // Basically _all_ writes in this function must come with some memory barrier. Be careful!
  // Also, the performance of this method really matters, especially that of common path.
  // Check objdump -d. Everything in common path should be inlined.
  // Also, check minimal sufficient lock/mfences.
  assert_mcs_aligned(mcs_lock);
  ASSERT_ND(!adaptor_.me_waiting()->load());
  ASSERT_ND(mcs_lock->is_locked());
  ASSERT_ND(block_index > 0);
  ASSERT_ND(adaptor_.get_cur_block() >= block_index);
  const thread::ThreadId id = adaptor_.get_my_id();
  const uint32_t myself = McsLock::to_int(id, block_index);
  uint32_t* address = &(mcs_lock->data_);
  McsBlock* block = adaptor_.get_ww_my_block(block_index);
  if (!block->has_successor()) {
    // okay, successor "seems" nullptr (not contended), but we have to make it sure with atomic CAS
    uint32_t expected = myself;
    assert_mcs_aligned(address);
    bool swapped = assorted::raw_atomic_compare_exchange_strong<uint32_t>(address, &expected, 0);
    if (swapped) {
      // we have just unset the locked flag, but someone else might have just acquired it,
      // so we can't put assertion here.
      ASSERT_ND(id == 0 || mcs_lock->get_tail_waiter() != id);
      ASSERT_ND(expected == myself);
      ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != myself);
      DVLOG(2) << "Okay, release a lock uncontended. me=" << id;
      return;
    }
    ASSERT_ND(expected != 0);
    ASSERT_ND(expected != kMcsGuestId);
    DVLOG(0) << "Interesting contention on MCS release. I thought it's null, but someone has just "
      " jumped in. me=" << id << ", mcs_lock=" << *mcs_lock;
    // wait for someone else to set the successor
    ASSERT_ND(mcs_lock->is_locked());
    if (UNLIKELY(!block->has_successor())) {
      spin_until([block]{ return block->has_successor_atomic(); });
    }
  }
  thread::ThreadId successor_id = block->get_successor_thread_id();
  DVLOG(1) << "Okay, I have a successor. me=" << id << ", succ=" << successor_id;
  ASSERT_ND(successor_id != id);
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != myself);

  ASSERT_ND(adaptor_.get_other_cur_block(successor_id) >= block->get_successor_block());
  ASSERT_ND(adaptor_.other_waiting(successor_id)->load());
  ASSERT_ND(mcs_lock->is_locked());

  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != myself);
  adaptor_.other_waiting(successor_id)->store(false, std::memory_order_release);
  ASSERT_ND(assorted::atomic_load_seq_cst<uint32_t>(address) != myself);
}

template <typename ADAPTOR>
void McsWwImpl<ADAPTOR>::ownerless_release(McsLock* mcs_lock) {
  // Basically _all_ writes in this function must come with some memory barrier. Be careful!
  // Also, the performance of this method really matters, especially that of common path.
  // Check objdump -d. Everything in common path should be inlined.
  // Also, check minimal sufficient mfences (note, xchg implies lock prefix. not a compiler's bug!).
  assert_mcs_aligned(mcs_lock);
  uint32_t* address = &(mcs_lock->data_);
  assert_mcs_aligned(address);
  ASSERT_ND(mcs_lock->is_locked());
  spin_until([address]{
    uint32_t old_int = kMcsGuestId;
    return assorted::raw_atomic_compare_exchange_weak<uint32_t>(address, &old_int, 0);
  });
  DVLOG(1) << "Okay, guest released the lock.";
}

////////////////////////////////////////////////////////////////////////////////
///
///      The Simple MCS-RW lock.
///  These need partial specialization on RW_BLOCK=McsRwSimpleBlock.
///  However, C++ standard doesn't allow function partial specialization.
///  We thus make it a class partial specialization as below. Stupid? Totally agree!
///
////////////////////////////////////////////////////////////////////////////////
template <typename ADAPTOR>
class McsImpl<ADAPTOR, McsRwSimpleBlock> {  // partial specialization for McsRwSimpleBlock
 public:
  McsBlockIndex acquire_try_rw_writer(McsRwLock* lock) {
    McsBlockIndex block_index = adaptor_.issue_new_block();
    bool success = retry_async_rw_writer(lock, block_index);
    return success ? block_index : 0;
  }

  McsBlockIndex acquire_try_rw_reader(McsRwLock* lock) {
    McsBlockIndex block_index = adaptor_.issue_new_block();
    bool success = retry_async_rw_reader(lock, block_index);
#ifndef NDEBUG
    if (success) {
      auto* my_block = adaptor_.get_rw_my_block(block_index);
      ASSERT_ND(my_block->is_finalized());
      ASSERT_ND(my_block->is_granted());
    }
#endif  // NDEBUG
    return success ? block_index : 0;
  }

  McsBlockIndex acquire_unconditional_rw_reader(McsRwLock* mcs_rw_lock) {
    ASSERT_ND(adaptor_.get_cur_block() < 0xFFFFU);
    const thread::ThreadId id = adaptor_.get_my_id();
    const McsBlockIndex block_index = adaptor_.issue_new_block();
    ASSERT_ND(block_index > 0);
    // TODO(tzwang): make this a static_size_check...
    ASSERT_ND(sizeof(McsRwSimpleBlock) == sizeof(McsBlock));
    auto* my_block = adaptor_.get_rw_my_block(block_index);

    // So I'm a reader
    my_block->init_reader();
    ASSERT_ND(my_block->is_blocked() && my_block->is_reader());
    ASSERT_ND(!my_block->has_successor());
    ASSERT_ND(my_block->successor_block_index_ == 0);

    // Now ready to XCHG
    uint32_t tail_desired = McsRwLock::to_tail_int(id, block_index);
    uint32_t* tail_address = &(mcs_rw_lock->tail_);
    uint32_t pred_tail_int = assorted::raw_atomic_exchange<uint32_t>(tail_address, tail_desired);

    if (pred_tail_int == 0) {
      mcs_rw_lock->increment_nreaders();
      my_block->unblock();  // reader successors will know they don't need to wait
    } else {
      // See if the predecessor is a reader; if so, if it already acquired the lock.
      auto* pred_block = adaptor_.dereference_rw_tail_block(pred_tail_int);
      uint16_t* pred_state_address = &pred_block->self_.data_;
      uint16_t pred_state_expected = pred_block->make_blocked_with_no_successor_state();
      uint16_t pred_state_desired = pred_block->make_blocked_with_reader_successor_state();
      if (!pred_block->is_reader() || assorted::raw_atomic_compare_exchange_strong<uint16_t>(
        pred_state_address,
        &pred_state_expected,
        pred_state_desired)) {
        // Predecessor is a writer or a waiting reader. The successor class field and the
        // blocked state in pred_block are separated, so we can blindly set_successor().
        pred_block->set_successor_next_only(id, block_index);
        spin_until([my_block]{ return my_block->is_granted(); });
      } else {
        // Join the active, reader predecessor
        ASSERT_ND(!pred_block->is_blocked());
        mcs_rw_lock->increment_nreaders();
        pred_block->set_successor_next_only(id, block_index);
        my_block->unblock();
      }
    }
    finalize_acquire_reader_simple(mcs_rw_lock, my_block);
    ASSERT_ND(my_block->is_finalized());
    return block_index;
  }

  void release_rw_reader(
    McsRwLock* mcs_rw_lock,
    McsBlockIndex block_index) {
    const thread::ThreadId id = adaptor_.get_my_id();
    ASSERT_ND(block_index > 0);
    ASSERT_ND(adaptor_.get_cur_block() >= block_index);
    McsRwSimpleBlock* my_block = adaptor_.get_rw_my_block(block_index);
    ASSERT_ND(my_block->is_finalized());
    // Make sure there is really no successor or wait for it
    uint32_t* tail_address = &mcs_rw_lock->tail_;
    uint32_t expected = McsRwLock::to_tail_int(id, block_index);
    if (my_block->successor_is_ready() ||
      !assorted::raw_atomic_compare_exchange_strong<uint32_t>(tail_address, &expected, 0)) {
      // Have to wait for the successor to install itself after me
      // Don't check for curr_block->has_successor()! It only tells whether the state bit
      // is set, not whether successor_thread_id_ and successor_block_index_ are set.
      // But remember to skip trying readers who failed.
      spin_until([my_block]{ return my_block->successor_is_ready(); });
      if (my_block->has_writer_successor()) {
        assorted::raw_atomic_exchange<thread::ThreadId>(
          &mcs_rw_lock->next_writer_,
          my_block->successor_thread_id_);
      }
    }

    if (mcs_rw_lock->decrement_nreaders() == 1) {
      // I'm the last active reader
      thread::ThreadId next_writer
        = assorted::atomic_load_acquire<thread::ThreadId>(&mcs_rw_lock->next_writer_);
      if (next_writer != McsRwLock::kNextWriterNone &&
          mcs_rw_lock->nreaders() == 0 &&
          assorted::raw_atomic_compare_exchange_strong<thread::ThreadId>(
            &mcs_rw_lock->next_writer_,
            &next_writer,
            McsRwLock::kNextWriterNone)) {
        // I have a waiting writer, wake it up
        // Assuming a thread can wait for one and only one MCS lock at any instant
        // before starting to acquire the next.
        McsBlockIndex next_cur_block = adaptor_.get_other_cur_block(next_writer);
        McsRwSimpleBlock *writer_block = adaptor_.get_rw_other_block(next_writer, next_cur_block);
        ASSERT_ND(writer_block->is_blocked());
        ASSERT_ND(!writer_block->is_reader());
        writer_block->unblock();
      }
    }
  }

  McsBlockIndex acquire_unconditional_rw_writer(
    McsRwLock* mcs_rw_lock) {
    const thread::ThreadId id = adaptor_.get_my_id();
    const McsBlockIndex block_index = adaptor_.issue_new_block();
    ASSERT_ND(adaptor_.get_cur_block() < 0xFFFFU);
    ASSERT_ND(block_index > 0);
    // TODO(tzwang): make this a static_size_check...
    ASSERT_ND(sizeof(McsRwSimpleBlock) == sizeof(McsBlock));
    auto* my_block = adaptor_.get_rw_my_block(block_index);

    my_block->init_writer();
    ASSERT_ND(my_block->is_blocked() && !my_block->is_reader());
    ASSERT_ND(!my_block->has_successor());
    ASSERT_ND(my_block->successor_block_index_ == 0);

    // Now ready to XCHG
    uint32_t tail_desired = McsRwLock::to_tail_int(id, block_index);
    uint32_t* tail_address = &(mcs_rw_lock->tail_);
    uint32_t pred_tail_int = assorted::raw_atomic_exchange<uint32_t>(tail_address, tail_desired);
    ASSERT_ND(pred_tail_int != tail_desired);
    thread::ThreadId old_next_writer = 0xFFFFU;
    if (pred_tail_int == 0) {
      ASSERT_ND(mcs_rw_lock->get_next_writer() == McsRwLock::kNextWriterNone);
      assorted::raw_atomic_exchange<thread::ThreadId>(&mcs_rw_lock->next_writer_, id);
      if (mcs_rw_lock->nreaders() == 0) {
        old_next_writer = assorted::raw_atomic_exchange<thread::ThreadId>(
          &mcs_rw_lock->next_writer_,
          McsRwLock::kNextWriterNone);
        if (old_next_writer == id) {
          my_block->unblock();
          return block_index;
        }
      }
    } else {
      auto* pred_block = adaptor_.dereference_rw_tail_block(pred_tail_int);
      pred_block->set_successor_class_writer();
      pred_block->set_successor_next_only(id, block_index);
    }
    spin_until([my_block]{ return my_block->is_granted(); });
    return block_index;
  }

  void release_rw_writer(
    McsRwLock* mcs_rw_lock,
    McsBlockIndex block_index) {
    const thread::ThreadId id = adaptor_.get_my_id();
    ASSERT_ND(block_index > 0);
    ASSERT_ND(adaptor_.get_cur_block() >= block_index);
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    uint32_t expected = McsRwLock::to_tail_int(id, block_index);
    uint32_t* tail_address = &mcs_rw_lock->tail_;
    if (my_block->successor_is_ready() ||
      !assorted::raw_atomic_compare_exchange_strong<uint32_t>(tail_address, &expected, 0)) {
      if (UNLIKELY(!my_block->successor_is_ready())) {
        spin_until([my_block]{ return my_block->successor_is_ready(); });
      }
      ASSERT_ND(my_block->successor_is_ready());
      auto* successor_block = adaptor_.get_rw_other_block(
        my_block->successor_thread_id_,
        my_block->successor_block_index_);
      ASSERT_ND(successor_block->is_blocked());
      if (successor_block->is_reader()) {
        mcs_rw_lock->increment_nreaders();
      }
      successor_block->unblock();
    }
  }


  AcquireAsyncRet acquire_async_rw_reader(McsRwLock* lock) {
    // In simple version, no distinction between try/async/retry. Same logic.
    McsBlockIndex block_index = adaptor_.issue_new_block();
    bool success = retry_async_rw_reader(lock, block_index);
    return {success, block_index};
  }
  AcquireAsyncRet acquire_async_rw_writer(McsRwLock* lock) {
    McsBlockIndex block_index = adaptor_.issue_new_block();
    bool success = retry_async_rw_writer(lock, block_index);
    return {success, block_index};
  }

  bool retry_async_rw_reader(
    McsRwLock* lock,
    McsBlockIndex block_index) {
    const thread::ThreadId id = adaptor_.get_my_id();
    // take a look at the whole lock word, and cas if it's a reader or null
    uint64_t lock_word
      = assorted::atomic_load_acquire<uint64_t>(reinterpret_cast<uint64_t*>(lock));
    McsRwLock ll;
    std::memcpy(&ll, &lock_word, sizeof(ll));
    // Note: it's tempting to put this whole function under an infinite retry
    // loop and only break when this condition is true. That works fine with
    // a single lock, but might cause deadlocks and making this try version
    // not really a try, consider this example with two locks A and B.
    //
    // Lock: requester 1 -> requester 2
    //
    // A: T1 holding as writer -> T2 waiting unconditionally as a writer in canonical mode
    // B: T2 holding as writer -> T1 trying as a reader in non-canonical mode
    //
    // In this case, T1 always sees next_writer=none because T2 consumed it when it got the
    // lock, and the below CAS fails because now B.tail is T2, a writer. T1 would stay in
    // the loop forever...
    if (ll.next_writer_ != McsRwLock::kNextWriterNone) {
      return false;
    }
    McsRwSimpleBlock* block = nullptr;
    if (ll.tail_) {
      block = adaptor_.dereference_rw_tail_block(ll.tail_);
    }
    if (ll.tail_ == 0 || (block->is_granted() && block->is_reader())) {
      ll.increment_nreaders();
      ll.tail_ = McsRwLock::to_tail_int(id, block_index);
      uint64_t desired = *reinterpret_cast<uint64_t*>(&ll);
      auto* my_block = adaptor_.get_rw_my_block(block_index);
      my_block->init_reader();

      if (assorted::raw_atomic_compare_exchange_weak<uint64_t>(
        reinterpret_cast<uint64_t*>(lock), &lock_word, desired)) {
        if (block) {
          block->set_successor_next_only(id, block_index);
        }
        my_block->unblock();
        finalize_acquire_reader_simple(lock, my_block);
        return true;
      }
    }
    return false;
  }
  bool retry_async_rw_writer(McsRwLock* lock, McsBlockIndex block_index) {
    const thread::ThreadId id = adaptor_.get_my_id();
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    my_block->init_writer();

    McsRwLock tmp;
    uint64_t expected = *reinterpret_cast<uint64_t*>(&tmp);
    McsRwLock tmp2;
    tmp2.tail_ = McsRwLock::to_tail_int(id, block_index);
    uint64_t desired = *reinterpret_cast<uint64_t*>(&tmp2);
    my_block->unblock();
    return assorted::raw_atomic_compare_exchange_weak<uint64_t>(
      reinterpret_cast<uint64_t*>(lock), &expected, desired);
  }

  void cancel_async_rw_reader(McsRwLock* /*lock*/, McsBlockIndex /*block_index*/) {
    // In simple version, we don't actually have any mechanism to retry.
    // so, we don't have to do any cancel, either. No-op.
  }
  void cancel_async_rw_writer(McsRwLock* /*lock*/, McsBlockIndex /*block_index*/) {
  }

 private:
  /** internal utility func used only in simple version of acquire_unconditional_rw_reader() */
  void finalize_acquire_reader_simple(McsRwLock* lock, McsRwSimpleBlock* my_block) {
    ASSERT_ND(!my_block->is_finalized());
    if (my_block->has_reader_successor()) {
      spin_until([my_block]{ return my_block->successor_is_ready(); });
      // Unblock the reader successor
      McsRwSimpleBlock* successor_block = adaptor_.get_rw_other_block(
        my_block->successor_thread_id_,
        my_block->successor_block_index_);
      lock->increment_nreaders();
      successor_block->unblock();
    }
    my_block->set_finalized();
  }

  ADAPTOR adaptor_;
};  // end of McsImpl<ADAPTOR, McsRwSimpleBlock> specialization

////////////////////////////////////////////////////////////////////////////////
///
///      The Extended MCS-RW lock.
///  Same as above, we partially specialize the whole class, not functions.
///
////////////////////////////////////////////////////////////////////////////////
template <typename ADAPTOR>
class McsImpl<ADAPTOR, McsRwExtendedBlock> {  // partial specialization for McsRwExtendedBlock
 public:
  McsBlockIndex acquire_unconditional_rw_reader(McsRwLock* lock) {
    McsBlockIndex block_index = 0;
    auto ret = acquire_reader_lock(lock, &block_index, McsRwExtendedBlock::kTimeoutNever);
    ASSERT_ND(block_index);
    ASSERT_ND(ret == kErrorCodeOk);
#ifndef NDEBUG
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    ASSERT_ND(my_block->next_flag_is_granted());
    ASSERT_ND(my_block->pred_flag_is_granted());
#endif
    return block_index;
  }
  McsBlockIndex acquire_unconditional_rw_writer(McsRwLock* lock) {
    McsBlockIndex block_index = 0;
    auto ret = acquire_writer_lock(lock, &block_index, McsRwExtendedBlock::kTimeoutNever);
    ASSERT_ND(block_index);
    ASSERT_ND(ret == kErrorCodeOk);
#ifndef NDEBUG
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    ASSERT_ND(my_block->next_flag_is_granted());
    ASSERT_ND(my_block->pred_flag_is_granted());
#endif
    return block_index;
  }
  /** Instant-try versions, won't push queue node if failed.
   * Same as acquire_try_rw_* in SimpleRWLock. */
  McsBlockIndex acquire_try_rw_writer(McsRwLock* lock) {
    const thread::ThreadId id = adaptor_.get_my_id();
    McsBlockIndex block_index = adaptor_.issue_new_block();
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    my_block->init_writer();

    McsRwLock tmp;
    uint64_t expected = *reinterpret_cast<uint64_t*>(&tmp);
    McsRwLock tmp2;
    tmp2.tail_ = McsRwLock::to_tail_int(id, block_index);
    uint64_t desired = *reinterpret_cast<uint64_t*>(&tmp2);
    my_block->set_pred_flag_granted();
    my_block->set_next_flag_granted();
    if (assorted::raw_atomic_compare_exchange_weak<uint64_t>(
      reinterpret_cast<uint64_t*>(lock), &expected, desired)) {
      return block_index;
    }
    return 0;
  }
  McsBlockIndex acquire_try_rw_reader(McsRwLock* lock) {
    McsBlockIndex block_index = adaptor_.issue_new_block();
    const thread::ThreadId id = adaptor_.get_my_id();
    while (true) {
      // take a look at the whole lock word, and cas if it's a reader or null
      uint64_t lock_word
        = assorted::atomic_load_acquire<uint64_t>(reinterpret_cast<uint64_t*>(lock));
      McsRwLock ll;
      std::memcpy(&ll, &lock_word, sizeof(ll));
      if (ll.next_writer_ != McsRwLock::kNextWriterNone) {
        return 0;
      }
      McsRwExtendedBlock* block = nullptr;
      if (ll.tail_) {
        block = adaptor_.dereference_rw_tail_block(ll.tail_);
      }
      if (ll.tail_ == 0 || (block->pred_flag_is_granted() && block->is_reader())) {
        ll.increment_nreaders();
        ll.tail_ = McsRwLock::to_tail_int(id, block_index);
        uint64_t desired = *reinterpret_cast<uint64_t*>(&ll);
        auto* my_block = adaptor_.get_rw_my_block(block_index);
        my_block->init_reader();

        if (assorted::raw_atomic_compare_exchange_weak<uint64_t>(
          reinterpret_cast<uint64_t*>(lock), &lock_word, desired)) {
          if (block) {
            block->set_next_id(McsRwExtendedBlock::kSuccIdNoSuccessor);
          }
          my_block->set_pred_flag_granted();
          finish_acquire_reader_lock(lock, my_block, ll.tail_);
          ASSERT_ND(my_block->pred_flag_is_granted());
          ASSERT_ND(my_block->next_flag_is_granted());
          return block_index;
        }
      }
    }
    ASSERT_ND(false);
  }
  void release_rw_reader(McsRwLock* lock, McsBlockIndex block_index) {
    release_reader_lock(lock, block_index);
  }
  void release_rw_writer(McsRwLock* lock, McsBlockIndex block_index) {
    release_writer_lock(lock, block_index);
  }
  /** Async acquire methods, passing timeout 0 will avoid cancelling upon timeout in
   * the internal rountines; caller should explicitly cancel when needed. */
  AcquireAsyncRet acquire_async_rw_reader(McsRwLock* lock) {
    McsBlockIndex block_index = 0;
    auto ret = acquire_reader_lock(lock, &block_index, McsRwExtendedBlock::kTimeoutZero);
    ASSERT_ND(ret == kErrorCodeOk || ret == kErrorCodeLockRequested);
#ifndef NDEBUG
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    if (ret == kErrorCodeOk) {
      ASSERT_ND(my_block->pred_flag_is_granted());
      ASSERT_ND(my_block->next_flag_is_granted());
    } else {
      ASSERT_ND(ret == kErrorCodeLockRequested);
      ASSERT_ND(!my_block->next_flag_is_granted());
    }
#endif
    ASSERT_ND(block_index);
    return {ret == kErrorCodeOk, block_index};
  }
  AcquireAsyncRet acquire_async_rw_writer(McsRwLock* lock) {
    McsBlockIndex block_index = 0;
    auto ret = acquire_writer_lock(lock, &block_index, McsRwExtendedBlock::kTimeoutZero);
    ASSERT_ND(ret == kErrorCodeOk || ret == kErrorCodeLockRequested);
#ifndef NDEBUG
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    if (ret == kErrorCodeOk) {
      ASSERT_ND(my_block->pred_flag_is_granted());
      ASSERT_ND(my_block->next_flag_is_granted());
    } else {
      ASSERT_ND(ret == kErrorCodeLockRequested);
      ASSERT_ND(!my_block->next_flag_is_granted());
    }
#endif
    ASSERT_ND(block_index);
    return {ret == kErrorCodeOk, block_index};
  }
  bool retry_async_rw_reader(McsRwLock* lock, McsBlockIndex block_index) {
    auto* block = adaptor_.get_rw_my_block(block_index);
    if (block->pred_flag_is_granted()) {
      // checking me.next.flags.granted is ok - we're racing with ourself
      if (!block->next_flag_is_granted()) {
        auto ret = finish_acquire_reader_lock(lock, block,
          xct::McsRwLock::to_tail_int(static_cast<uint32_t>(adaptor_.get_my_id()), block_index));
        ASSERT_ND(ret == kErrorCodeOk);
      }
      ASSERT_ND(block->next_flag_is_granted());
      return true;
    }
    ASSERT_ND(!block->next_flag_is_granted());
    return false;
  }
  bool retry_async_rw_writer(McsRwLock* /*lock*/, McsBlockIndex block_index) {
    auto* block = adaptor_.get_rw_my_block(block_index);
    if (block->pred_flag_is_granted()) {
      // checking me.next.flags.granted is ok - we're racing with ourself
      if (!block->next_flag_is_granted()) {
        block->set_next_flag_granted();
      }
      ASSERT_ND(block->next_flag_is_granted());
      return true;
    }
    ASSERT_ND(!block->next_flag_is_granted());
    return false;
  }
  void cancel_async_rw_reader(McsRwLock* lock, McsBlockIndex block_index) {
    if (!retry_async_rw_reader(lock, block_index)) {
      uint32_t my_tail_int = McsRwLock::to_tail_int(adaptor_.get_my_id(), block_index);
      if (cancel_reader_lock(lock, my_tail_int) == kErrorCodeOk) {
        // actually got the lock, have to release then
        release_reader_lock(lock, block_index);
      }
    } else {
      release_reader_lock(lock, block_index);
    }
  }
  void cancel_async_rw_writer(McsRwLock* lock, McsBlockIndex block_index) {
    uint32_t my_tail_int = McsRwLock::to_tail_int(adaptor_.get_my_id(), block_index);
    if (cancel_writer_lock(lock, my_tail_int) == kErrorCodeOk) {
      release_writer_lock(lock, block_index);
    }
  }

 private:
  /** internal utility functions for extended rw-lock. */
  McsRwExtendedBlock* init_block(xct::McsBlockIndex* out_block_index, bool writer) {
    ASSERT_ND(out_block_index);
    McsBlockIndex block_index = 0;
    if (*out_block_index) {
      // already provided, use it; caller must make sure this block is not being used
      block_index = *out_block_index;
    } else {
      block_index = *out_block_index = adaptor_.issue_new_block();
    }
    ASSERT_ND(block_index <= 0xFFFFU);
    ASSERT_ND(block_index > 0);
    ASSERT_ND(adaptor_.get_cur_block() < 0xFFFFU);
    auto* my_block = adaptor_.get_rw_my_block(block_index);
    if (writer) {
      my_block->init_writer();
    } else {
      my_block->init_reader();
    }
    return my_block;
  }

  ErrorCode acquire_reader_lock(McsRwLock* lock, McsBlockIndex* out_block_index, int32_t timeout) {
    auto* my_block = init_block(out_block_index, false);
    ASSERT_ND(my_block->pred_flag_is_waiting());
    ASSERT_ND(my_block->next_flag_is_waiting());
    ASSERT_ND(!my_block->next_flag_is_busy());
    const thread::ThreadId id = adaptor_.get_my_id();
    auto my_tail_int = McsRwLock::to_tail_int(id, *out_block_index);

    auto pred = lock->xchg_tail(my_tail_int);
    if (pred == 0) {
      lock->increment_nreaders();
      ASSERT_ND(my_block->get_pred_id() == 0);
      my_block->set_pred_flag_granted();
      return finish_acquire_reader_lock(lock, my_block, my_tail_int);
    }

    ASSERT_ND(my_block->get_pred_id() == 0);
    // haven't set pred.next.id yet, safe to dereference
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
    if (pred_block->is_reader()) {
      return acquire_reader_lock_check_reader_pred(lock, my_block, my_tail_int, pred, timeout);
    }
    return acquire_reader_lock_check_writer_pred(lock, my_block, my_tail_int, pred, timeout);
  }

  ErrorCode finish_acquire_reader_lock(
    McsRwLock* lock, McsRwExtendedBlock* my_block, uint32_t my_tail_int) {
    my_block->set_next_flag_busy_granted();
    ASSERT_ND(my_block->next_flag_is_granted());
    ASSERT_ND(my_block->next_flag_is_busy());
    spin_until([my_block]{
      return my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving; });

    // if the lock tail now still points to me, truly no one is there, we're done
    if (lock->get_tail_int() == my_tail_int) {
      my_block->unset_next_flag_busy();
      return kErrorCodeOk;
    }
    // note that the successor can't cancel now, ie my next.id is stable
    spin_until([my_block]{ return my_block->get_next_id() != 0; });
    uint64_t next = my_block->get_next();
    uint32_t next_id = next >> 32;
    ASSERT_ND(next_id);
    ASSERT_ND(next_id != McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    ASSERT_ND(my_block->next_flag_is_granted());
    ASSERT_ND(my_block->next_flag_is_busy());
    if (next_id == McsRwExtendedBlock::kSuccIdNoSuccessor) {
      my_block->unset_next_flag_busy();
      return kErrorCodeOk;
    }

    auto* succ_block = adaptor_.dereference_rw_tail_block(next_id);
    if (my_block->next_flag_is_leaving_granted() && !my_block->next_flag_has_successor()) {
      // successor might have seen me in leaving state, it'll wait for me in that case
      // in this case, the successor saw me in leaving state and didnt register as a reader
      // ie successor was acquiring
      spin_until([succ_block, my_tail_int]{ return succ_block->get_pred_id() == my_tail_int; });
      ASSERT_ND(succ_block->pred_flag_is_waiting());
      if (succ_block->cas_pred_id_weak(my_tail_int, McsRwExtendedBlock::kPredIdAcquired)) {
        lock->increment_nreaders();
        succ_block->set_pred_flag_granted();
        // make sure I know when releasing no need to wait
        my_block->set_next_id(McsRwExtendedBlock::kSuccIdNoSuccessor);
      }
    } else {
      if (my_block->next_flag_has_reader_successor()) {
        while (true) {
          spin_until([succ_block, my_tail_int]{ return succ_block->get_pred_id() == my_tail_int; });
          if (succ_block->cas_pred_id_weak(my_tail_int, McsRwExtendedBlock::kPredIdAcquired)) {
            ASSERT_ND(succ_block->pred_flag_is_waiting());
            lock->increment_nreaders();
            succ_block->set_pred_flag_granted();
            my_block->set_next_id(McsRwExtendedBlock::kSuccIdNoSuccessor);
            break;
          }
        }
      }
    }
    my_block->unset_next_flag_busy();
    return kErrorCodeOk;
  }

  ErrorCode acquire_reader_lock_check_reader_pred(
    McsRwLock* lock,
    McsRwExtendedBlock* my_block,
    uint32_t my_tail_int,
    uint32_t pred,
    int32_t timeout) {
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
  check_pred:
    ASSERT_ND(my_block->get_pred_id() == 0);
    ASSERT_ND(pred_block->is_reader());
    // wait for the previous canceling dude to leave
    spin_until([pred_block]{
      return !pred_block->get_next_id() && !pred_block->next_flag_has_successor(); });
    uint32_t expected = pred_block->make_next_flag_waiting_with_no_successor();
    uint32_t val = pred_block->cas_val_next_flag_weak(
      expected, pred_block->make_next_flag_waiting_with_reader_successor());
    if (val == expected) {
      pred_block->set_next_id(my_tail_int);
      my_block->set_pred_id(pred);
      if (my_block->timeout_granted(timeout)) {
        return finish_acquire_reader_lock(lock, my_block, my_tail_int);
      }
      if (timeout == McsRwExtendedBlock::kTimeoutZero) {
        return kErrorCodeLockRequested;
      }
      return cancel_reader_lock(lock, my_tail_int);
    }

    if ((val & McsRwExtendedBlock::kSuccFlagMask) == McsRwExtendedBlock::kSuccFlagLeaving) {
      // don't set pred.next.successor_class here
      pred_block->set_next_id(my_tail_int);
      my_block->set_pred_id(pred);
      // if pred did cancel, it will give me a new pred; if it got the lock it will wake me up
      spin_until([my_block, pred]{
        return my_block->get_pred_id() != pred || !my_block->pred_flag_is_waiting(); });
      // consume it and retry
      pred = my_block->xchg_pred_id(0);
      if (pred == McsRwExtendedBlock::kPredIdAcquired) {
        spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
        return finish_acquire_reader_lock(lock, my_block, my_tail_int);
      }
      ASSERT_ND(!my_block->pred_flag_is_granted());
      ASSERT_ND(pred);
      ASSERT_ND(pred != McsRwExtendedBlock::kPredIdAcquired);
      pred_block = adaptor_.dereference_rw_tail_block(pred);
      if (pred_block->is_writer()) {
        return acquire_reader_lock_check_writer_pred(lock, my_block, my_tail_int, pred, timeout);
      }
      goto check_pred;
    } else {
      // pred is granted - might be a direct grant or grant in the leaving process
      ASSERT_ND(
        (val & McsRwExtendedBlock::kSuccFlagMask) == McsRwExtendedBlock::kSuccFlagDirectGranted ||
        (val & McsRwExtendedBlock::kSuccFlagMask) == McsRwExtendedBlock::kSuccFlagLeavingGranted);
      if (pred_block->is_reader()) {
        // I didn't register, pred won't wake me up, but if pred is leaving_granted,
        // we need to tell it not to poke me in its finish-acquire call. For direct_granted,
        // also set its next.id to NoSuccessor so it knows that there's no need to wait and
        // examine successor upon release. This also covers the case when pred.next.flags
        // has Busy set.
        pred_block->set_next_id(McsRwExtendedBlock::kSuccIdNoSuccessor);
        lock->increment_nreaders();
        my_block->set_pred_flag_granted();
        return finish_acquire_reader_lock(lock, my_block, my_tail_int);
      } else {
        my_block->set_pred_id(pred);
        pred_block->set_next_id(my_tail_int);
        if (my_block->timeout_granted(timeout)) {
          return finish_acquire_reader_lock(lock, my_block, my_tail_int);
        }
        if (timeout == McsRwExtendedBlock::kTimeoutZero) {
          return kErrorCodeLockRequested;
        }
        return cancel_reader_lock(lock, my_tail_int);
      }
    }
    ASSERT_ND(false);
  }

  ErrorCode cancel_reader_lock(McsRwLock* lock, uint32_t my_tail_int) {
    auto* my_block = adaptor_.dereference_rw_tail_block(my_tail_int);
    auto pred = my_block->xchg_pred_id(0);  // prevent pred from cancelling
    if (pred == McsRwExtendedBlock::kPredIdAcquired) {
      spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
      return finish_acquire_reader_lock(lock, my_block, my_tail_int);
    }

    // make sure successor can't leave, unless it tried to leave first
    ASSERT_ND(!my_block->next_flag_is_granted());
    my_block->set_next_flag_leaving();
    spin_until([my_block]{
      return my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving; });

    ASSERT_ND(pred);
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
    if (pred_block->is_reader()) {
      return cancel_reader_lock_with_reader_pred(lock, my_block, my_tail_int, pred);
    }
    ASSERT_ND(my_block->get_pred_id() == 0);
    return cancel_reader_lock_with_writer_pred(lock, my_block, my_tail_int, pred);
  }

  ErrorCode cancel_reader_lock_with_writer_pred(
    McsRwLock* lock, McsRwExtendedBlock* my_block, uint32_t my_tail_int, uint32_t pred) {
  retry:
    ASSERT_ND(my_block->next_flag_is_leaving());
    ASSERT_ND(pred);
    ASSERT_ND(pred >> 16 != adaptor_.get_my_id());
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
    ASSERT_ND(pred_block->is_writer());
    ASSERT_ND(my_block->get_pred_id() == 0);
    // wait for the cancelling pred to finish relink
    spin_until([pred_block, my_tail_int]{
      return pred_block->get_next_id() == my_tail_int &&
        pred_block->next_flag_has_reader_successor(); });
    ASSERT_ND(pred_block->next_flag_has_reader_successor());
    // pred is a writer, so I can go as long as it's not also leaving (cancelling or releasing)
    ASSERT_ND(my_block->get_pred_id() == 0);
    while (true) {
      uint64_t eflags = pred_block->read_next_flags();
      if ((eflags & McsRwExtendedBlock::kSuccFlagMask) ==
        McsRwExtendedBlock::kSuccFlagLeaving) {
        // must wait for pred to give me a new pred (or wait to be waken up?)
        // pred should give me a new pred, after its CAS trying to pass me the lock failed
        ASSERT_ND(my_block->get_pred_id() == 0);
        my_block->set_pred_id(pred);
        spin_until([my_block, pred]{ return my_block->get_pred_id() != pred; });
        pred = my_block->xchg_pred_id(0);
        if (pred == McsRwExtendedBlock::kPredIdAcquired) {
          spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
          return finish_acquire_reader_lock(lock, my_block, my_tail_int);
        }
        ASSERT_ND(pred);
        pred_block = adaptor_.dereference_rw_tail_block(pred);
        if (pred_block->is_writer()) {
          goto retry;
        }
        return cancel_reader_lock_with_reader_pred(lock, my_block, my_tail_int, pred);
      } else if (eflags & McsRwExtendedBlock::kSuccFlagBusy) {
        ASSERT_ND(pred_block->next_flag_is_granted());
        ASSERT_ND(pred_block->next_flag_is_busy());
        my_block->set_pred_id(pred);
        spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
        return finish_acquire_reader_lock(lock, my_block, my_tail_int);
      }
      // try to tell pred I'm leaving
      if (pred_block->cas_next_weak(eflags | (static_cast<uint64_t>(my_tail_int) << 32),
        eflags | (static_cast<uint64_t>(McsRwExtendedBlock::kSuccIdSuccessorLeaving) << 32))) {
        break;
      }
    }
    // pred now has SuccessorLeaving on its next.id, it won't try to wake me up during release
    // now link the new successor and pred
    if (my_block->get_next_id() == 0 && lock->cas_tail_weak(my_tail_int, pred)) {
      pred_block->set_next_flag_no_successor();
      pred_block->set_next_id(0);
      ASSERT_ND(!my_block->next_flag_has_successor());
      return kErrorCodeLockCancelled;
    }

    cancel_reader_lock_relink(pred_block, my_block, my_tail_int, pred);
    return kErrorCodeLockCancelled;
  }

  ErrorCode cancel_reader_lock_with_reader_pred(
    McsRwLock* lock, McsRwExtendedBlock* my_block, uint32_t my_tail_int, uint32_t pred) {
  retry:
    ASSERT_ND(my_block->next_flag_is_leaving());
    // now successor can't attach to me assuming I'm waiting or has already done so.
    // CAS out of pred.next (including id and flags)
    ASSERT_ND(pred);
    ASSERT_ND(pred >> 16 != adaptor_.get_my_id());
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
    // wait for the canceling pred to finish the relink
    spin_until([pred_block, my_tail_int]{
      return pred_block->next_flag_has_reader_successor() &&
        pred_block->get_next_id() == my_tail_int; });

    uint64_t expected = pred_block->make_next_flag_waiting_with_reader_successor() |
      (static_cast<uint64_t>(my_tail_int) << 32);
    // only want to put SuccessorLeaving in the id field
    uint64_t desired = pred_block->make_next_flag_waiting_with_reader_successor() |
      (static_cast<uint64_t>(McsRwExtendedBlock::kSuccIdSuccessorLeaving) << 32);
    auto val = pred_block->cas_val_next_weak(expected, desired);
    ASSERT_ND(val & McsRwExtendedBlock::kSuccFlagSuccessorClassMask);
    if (val != expected) {
      // Note: we once registered after pred as a reader successor (still are), so if
      // pred happens to get the lock, it will wake me up seeing its reader_successor set
      auto pred_succ_flag = val & McsRwExtendedBlock::kSuccFlagMask;
      if (pred_succ_flag == McsRwExtendedBlock::kSuccFlagDirectGranted ||
        pred_succ_flag == McsRwExtendedBlock::kSuccFlagLeavingGranted) {
        // pred will in its finish-acquire-reader() wake me up.
        // pred already should alredy have me on its next.id, just set me.pred.id
        // this also covers the case when pred.next.flags has busy set.
        my_block->set_pred_id(pred);
        my_block->timeout_granted(McsRwExtendedBlock::kTimeoutNever);
        return finish_acquire_reader_lock(lock, my_block, my_tail_int);
      } else {
        ASSERT_ND(
          (val & McsRwExtendedBlock::kSuccFlagMask) == McsRwExtendedBlock::kSuccFlagLeaving);
        // pred is trying to leave, wait for a new pred or being waken up
        // pred has higher priority to leave, and it should already have me on its next.id
        my_block->set_pred_id(pred);
        spin_until([my_block, pred]{
          return my_block->get_pred_id() != pred || !my_block->pred_flag_is_waiting(); });
        // consume it and retry
        pred = my_block->xchg_pred_id(0);
        if (pred == McsRwExtendedBlock::kPredIdAcquired) {
          spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
          return finish_acquire_reader_lock(lock, my_block, my_tail_int);
        }
        pred_block = adaptor_.dereference_rw_tail_block(pred);
        ASSERT_ND(!my_block->pred_flag_is_granted());
        ASSERT_ND(pred);
        if (pred_block->is_writer()) {
          return cancel_reader_lock_with_writer_pred(lock, my_block, my_tail_int, pred);
        }
        goto retry;
      }
    } else {
      // at this point pred will be waiting for a new successor if it decides
      // to move and successor will be waiting for a new pred
      ASSERT_ND(my_block->next_flag_is_leaving());
      if (!my_block->next_flag_has_successor() && lock->cas_tail_weak(my_tail_int, pred)) {
        // newly arriving successor for this pred will wait
        // for the SuccessorLeaving mark to go away before trying the CAS
        ASSERT_ND(my_block->get_next_id() == 0);
        ASSERT_ND(my_block->next_flag_is_leaving());
        ASSERT_ND(!my_block->next_flag_has_successor());
        ASSERT_ND(pred_block->get_next_id() == McsRwExtendedBlock::kSuccIdSuccessorLeaving);
        pred_block->set_next_flag_no_successor();
        pred_block->set_next_id(0);
        return kErrorCodeLockCancelled;
      }

      cancel_reader_lock_relink(pred_block, my_block, my_tail_int, pred);
    }
    return kErrorCodeLockCancelled;
  }

  void cancel_reader_lock_relink(
    McsRwExtendedBlock* pred_block,
    McsRwExtendedBlock* my_block,
    uint32_t my_tail_int,
    uint32_t pred) {
    spin_until([my_block]{ return my_block->get_next_id() != 0; });
    ASSERT_ND(my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    ASSERT_ND(my_block->next_flag_is_leaving());
    uint32_t next_id = my_block->get_next_id();
    ASSERT_ND(next_id);
    ASSERT_ND(next_id != McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    auto* succ_block = adaptor_.dereference_rw_tail_block(next_id);
    ASSERT_ND(pred);
    while (!succ_block->cas_pred_id_weak(my_tail_int, pred)) {}

    uint64_t successor = 0;
    if (my_block->next_flag_has_reader_successor()) {
      successor = static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagSuccessorReader) |
        (static_cast<uint64_t>(next_id) << 32);
    } else if (my_block->next_flag_has_writer_successor()) {
      successor = static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagSuccessorWriter) |
        (static_cast<uint64_t>(next_id) << 32);
    }
    ASSERT_ND(pred_block->next_flag_has_reader_successor());
    ASSERT_ND(pred_block->get_next_id() == McsRwExtendedBlock::kSuccIdSuccessorLeaving);

    uint64_t expected = 0, new_next = 0;
    do {  // preserve pred.flags
      expected = pred_block->get_next();
      new_next = successor | (expected & static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagMask));
      if (expected & static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagBusy)) {
        new_next |= static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagBusy);
      }
      ASSERT_ND((expected >> 32) == McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    } while (!pred_block->cas_next_weak(expected, new_next));
  }

  ErrorCode acquire_reader_lock_check_writer_pred(
    McsRwLock* lock,
    McsRwExtendedBlock* my_block,
    uint32_t my_tail_int,
    uint32_t pred,
    int32_t timeout) {
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
    ASSERT_ND(pred_block->is_writer());
    // wait for the previous canceling dude to leave
    spin_until([pred_block]{
      return !pred_block->get_next_id() && !pred_block->next_flag_has_successor(); });
    // pred is a writer, we have to wait anyway, so register and wait with timeout
    ASSERT_ND(my_block->get_pred_id() == 0);
    pred_block->set_next_flag_reader_successor();
    pred_block->set_next_id(my_tail_int);
    if (my_block->xchg_pred_id(pred) == McsRwExtendedBlock::kPredIdAcquired) {
      timeout = McsRwExtendedBlock::kTimeoutNever;
    }

    if (my_block->timeout_granted(timeout)) {
      return finish_acquire_reader_lock(lock, my_block, my_tail_int);
    }
    if (timeout == McsRwExtendedBlock::kTimeoutZero) {
      return kErrorCodeLockRequested;
    }
    return cancel_reader_lock(lock, my_tail_int);
  }

  void release_reader_lock(McsRwLock* lock, McsBlockIndex block_index) {
    auto id = adaptor_.get_my_id();
    auto my_tail_int = McsRwLock::to_tail_int(id, block_index);
    auto* my_block = adaptor_.get_rw_other_block(id, block_index);

    // make sure successor can't leave; readers, however, can still get the lock as usual
    // by seeing me.next.flags.granted set
    ASSERT_ND(my_block->next_flag_is_granted());
    my_block->set_next_flag_busy();
    spin_until([my_block]{
      return my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving; });

    uint32_t next_id = my_block->get_next_id();
    while (next_id == 0) {
      if (lock->cas_tail_weak(my_tail_int, 0)) {  // really no one behind me
        finish_release_reader_lock(lock);
        return;
      }
      next_id = my_block->get_next_id();
    }

    ASSERT_ND(next_id);
    ASSERT_ND(next_id != McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    if (next_id != McsRwExtendedBlock::kSuccIdNoSuccessor) {  // already handled successor
      auto* succ_block = adaptor_.dereference_rw_tail_block(next_id);
      ASSERT_ND(my_block->next_flag_has_successor());
      ASSERT_ND(!succ_block->pred_flag_is_granted());
      if (succ_block->is_reader()) {
        // so a cancelled successor gave me this new successor
        ASSERT_ND(my_block->next_flag_is_busy());
        lock->increment_nreaders();
        while (!succ_block->cas_pred_id_weak(my_tail_int, McsRwExtendedBlock::kPredIdAcquired)) {}
        succ_block->set_pred_flag_granted();
      } else {
        ASSERT_ND(succ_block->is_writer());
        ASSERT_ND(my_block->next_flag_has_writer_successor());
        // put it in next_writer
        ASSERT_ND(!lock->has_next_writer());
        auto next_writer_id = next_id >> 16;
        lock->set_next_writer(next_writer_id);
        // also tell successor it doesn't have pred any more
        spin_until([succ_block, my_tail_int]{
          return succ_block->cas_pred_id_weak(my_tail_int, 0); });
      }
    }
    finish_release_reader_lock(lock);
  }

  void finish_release_reader_lock(McsRwLock* lock) {
    if (lock->decrement_nreaders() > 1) {
      return;
    }
    auto next_writer_id = lock->get_next_writer();
    if (next_writer_id != McsRwLock::kNextWriterNone &&
      lock->nreaders() == 0 &&
      lock->cas_next_writer_weak(next_writer_id, McsRwLock::kNextWriterNone)) {
      McsBlockIndex next_cur_block = adaptor_.get_other_cur_block(next_writer_id);
      auto* wb = adaptor_.get_rw_other_block(next_writer_id, next_cur_block);
      ASSERT_ND(!wb->pred_flag_is_granted());
      while (!wb->cas_pred_id_weak(0, McsRwExtendedBlock::kPredIdAcquired)) {}
      ASSERT_ND(lock->nreaders() == 0);
      wb->set_pred_flag_granted();
    }
  }

  ErrorCode acquire_writer_lock(
    McsRwLock* lock, McsBlockIndex* out_block_index, int32_t timeout) {
    auto* my_block = init_block(out_block_index, true);
    ASSERT_ND(my_block->is_writer());
    auto id = adaptor_.get_my_id();
    auto my_tail_int = McsRwLock::to_tail_int(id, *out_block_index);
    auto pred = lock->xchg_tail(my_tail_int);
    if (pred == 0) {
      ASSERT_ND(lock->get_next_writer() == McsRwLock::kNextWriterNone);
      lock->set_next_writer(id);
      if (lock->nreaders() == 0) {
        if (lock->xchg_next_writer(McsRwLock::kNextWriterNone) == id) {
          my_block->set_flags_granted();
          ASSERT_ND(lock->nreaders() == 0);
          ASSERT_ND(lock->get_next_writer() == McsRwLock::kNextWriterNone);
          ASSERT_ND(my_block->next_flag_is_granted());
          return kErrorCodeOk;
        }
      }
    } else {
      auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
      spin_until([pred_block]{
        return !pred_block->next_flag_has_successor() && !pred_block->get_next_id(); });
      // register on pred.flags as a writer successor, then fill in pred.next.id and wait
      // must register on pred.flags first
      pred_block->set_next_flag_writer_successor();
      pred_block->set_next_id(my_tail_int);
    }

    if (my_block->xchg_pred_id(pred) == McsRwExtendedBlock::kPredIdAcquired) {
      timeout = McsRwExtendedBlock::kTimeoutNever;
    }

    if (my_block->timeout_granted(timeout)) {
      my_block->set_next_flag_granted();
      ASSERT_ND(lock->nreaders() == 0);
      ASSERT_ND(lock->get_next_writer() == McsRwLock::kNextWriterNone);
      ASSERT_ND(my_block->next_flag_is_granted());
      return kErrorCodeOk;
    }
    if (timeout == McsRwExtendedBlock::kTimeoutZero) {
      return kErrorCodeLockRequested;
    }
    return cancel_writer_lock(lock, my_tail_int);
  }

  void release_writer_lock(McsRwLock* lock, McsBlockIndex block_index) {
    auto id = adaptor_.get_my_id();
    auto my_tail_int = McsRwLock::to_tail_int(id, block_index);
    auto* my_block = adaptor_.get_rw_my_block(block_index);

    ASSERT_ND(my_block->next_flag_is_granted());
    ASSERT_ND(lock->nreaders() == 0);
    ASSERT_ND(lock->get_next_writer() == McsRwLock::kNextWriterNone);
    ASSERT_ND(my_block->pred_flag_is_granted());
    ASSERT_ND(my_block->next_flag_is_granted());
    my_block->set_next_flag_busy();  // make sure succesor can't leave
    spin_until([my_block]{
      return my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving; });
    ASSERT_ND(my_block->pred_flag_is_granted());
    ASSERT_ND(my_block->next_flag_is_granted());
    ASSERT_ND(my_block->next_flag_is_busy());
    ASSERT_ND(lock->nreaders() == 0);

    uint32_t next_id = my_block->get_next_id();
    while (next_id == 0) {
      if (lock->cas_tail_weak(my_tail_int, 0)) {
        return;
      }
      next_id = my_block->get_next_id();
    }
    ASSERT_ND(lock->nreaders() == 0);
    ASSERT_ND(my_block->next_flag_has_successor());
    ASSERT_ND(next_id);
    ASSERT_ND(next_id != McsRwExtendedBlock::kSuccIdSuccessorLeaving);

    auto* succ_block = adaptor_.dereference_rw_tail_block(next_id);
    ASSERT_ND(lock->nreaders() == 0);
    ASSERT_ND(!succ_block->pred_flag_is_granted());
    ASSERT_ND(succ_block->get_pred_id() != McsRwExtendedBlock::kPredIdAcquired);
    while (!succ_block->cas_pred_id_weak(my_tail_int, McsRwExtendedBlock::kPredIdAcquired)) {
      ASSERT_ND(my_block->get_next_id() == next_id);
    }
    if (succ_block->is_reader()) {
      lock->increment_nreaders();
    }
    succ_block->set_pred_flag_granted();
  }

  ErrorCode cancel_writer_lock(McsRwLock* lock, uint32_t my_tail_int) {
    auto* my_block = adaptor_.dereference_rw_tail_block(my_tail_int);
    auto pred = my_block->xchg_pred_id(0);
    // if pred is a releasing writer and already dereference my id, it will CAS me.pred.id
    // to Acquired, so we do a final check here; there's no way back after this point
    // (unless pred is a reader and it's already gone).
    // After my xchg, pred will be waiting for me to give it a new successor.
    if (pred == McsRwExtendedBlock::kPredIdAcquired) {
      spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
      my_block->set_next_flag_granted();
      ASSERT_ND(lock->nreaders() == 0);
      return kErrorCodeOk;
    }

    // "freeze" the successor
    my_block->set_next_flag_leaving();
    ASSERT_ND(!my_block->next_flag_is_granted());
    spin_until([my_block]{
      return my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving; });

    // if I still have a pred, then deregister from it; if I don't have a pred,
    // that means my pred has put me on next_writer, deregister from there and go
    // Note that the reader should first reset me.pred.id, then put me on lock.nw
    if (pred == 0) {
      return cancel_writer_lock_no_pred(lock, my_block, my_tail_int);
    }
    ASSERT_ND(pred);
    auto* pred_block = adaptor_.dereference_rw_tail_block(pred);
    while (true) {
      // wait for cancelling pred to finish relink, note pred_block is updated
      // later in the if block as well
      spin_until([pred_block, my_tail_int]{
        return pred_block->get_next_id() == my_tail_int &&
          pred_block->next_flag_has_writer_successor(); });
      // whatever flags value it might have, just not Leaving
      uint64_t eflags = pred_block->read_next_flags();
      if ((eflags & McsRwExtendedBlock::kSuccFlagMask) == McsRwExtendedBlock::kSuccFlagLeaving) {
        ASSERT_ND(my_block->get_pred_id() == 0);
        // pred might be cancelling (reader/writer) or releasing, so just wait
        my_block->set_pred_id(pred);
        spin_until([my_block, pred]{ return my_block->get_pred_id() != pred; });
        pred = my_block->xchg_pred_id(0);
        if (pred == 0) {
          // pred reader was releasing and it should have put me on lock.next_writer
          return cancel_writer_lock_no_pred(lock, my_block, my_tail_int);
        } else if (pred == McsRwExtendedBlock::kPredIdAcquired) {
          spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
          my_block->set_next_flag_granted();
          ASSERT_ND(lock->nreaders() == 0);
          return kErrorCodeOk;
        }
        pred_block = adaptor_.dereference_rw_tail_block(pred);
        continue;
      } else if (eflags & static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagBusy)) {
        // pred is perhaps releasing (writer)? me.pred.id is 0, pred can do nothing about me,
        // so it's safe to dereference
        if (pred_block->is_writer()) {
          ASSERT_ND(pred_block->get_next_id() == my_tail_int);
          my_block->set_pred_id(pred);
          spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
          ASSERT_ND(my_block->get_pred_id() == McsRwExtendedBlock::kPredIdAcquired);
          my_block->set_next_flag_granted();
          ASSERT_ND(lock->nreaders() == 0);
          return kErrorCodeOk;
        }
        ASSERT_ND(pred_block->is_reader());
        my_block->set_pred_id(pred);
        pred = my_block->xchg_pred_id(0);
        if (pred == 0) {
          return cancel_writer_lock_no_pred(lock, my_block, my_tail_int);
        } else if (pred == McsRwExtendedBlock::kPredIdAcquired) {
          spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
          my_block->set_next_flag_granted();
          ASSERT_ND(lock->nreaders() == 0);
          return kErrorCodeOk;
        }
        pred_block = adaptor_.dereference_rw_tail_block(pred);
        continue;  // retry if it's a reader
      }
      ASSERT_ND(pred_block->get_next_id() == my_tail_int);
      uint64_t desired = eflags |
        (static_cast<uint64_t>(McsRwExtendedBlock::kSuccIdSuccessorLeaving) << 32);
      uint64_t expected = eflags | (static_cast<uint64_t>(my_tail_int) << 32);
      ASSERT_ND(
        (expected & McsRwExtendedBlock::kSuccFlagMask) != McsRwExtendedBlock::kSuccFlagLeaving);
      auto val = pred_block->cas_val_next_weak(expected, desired);
      if (val == expected) {
        ASSERT_ND(pred_block->get_next_id() == McsRwExtendedBlock::kSuccIdSuccessorLeaving);
        break;
      }
    }

    ASSERT_ND(pred_block->get_next_id() == McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    if (!my_block->get_next_id() && lock->cas_tail_weak(my_tail_int, pred)) {
      pred_block->set_next_flag_no_successor();
      pred_block->set_next_id(0);
      return kErrorCodeLockCancelled;
    }
    spin_until([my_block]{ return my_block->get_next_id() != 0; });
    ASSERT_ND(my_block->get_next_id() != McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    ASSERT_ND(my_block->next_flag_is_leaving());
    uint32_t new_next_id = my_block->get_next_id();
    ASSERT_ND(new_next_id);
    ASSERT_ND(new_next_id != McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    auto* succ_block = adaptor_.dereference_rw_tail_block(new_next_id);
    while (!succ_block->cas_pred_id_weak(my_tail_int, pred)) {}

    uint64_t successor = 0;
    if (my_block->next_flag_has_reader_successor()) {
      successor = static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagSuccessorReader) |
        (static_cast<uint64_t>(new_next_id) << 32);
    } else if (my_block->next_flag_has_writer_successor()) {
      successor = static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagSuccessorWriter) |
        (static_cast<uint64_t>(new_next_id) << 32);
    }
    ASSERT_ND(pred_block->next_flag_has_writer_successor());
    ASSERT_ND(pred_block->get_next_id() == McsRwExtendedBlock::kSuccIdSuccessorLeaving);

  retry:
    // preserve pred.flags
    uint64_t expected = 0, new_next = 0;
    expected = pred_block->get_next();
    ASSERT_ND(expected >> 32 == McsRwExtendedBlock::kSuccIdSuccessorLeaving);
    new_next = (successor | static_cast<uint64_t>(expected & McsRwExtendedBlock::kSuccFlagMask));
    if (expected & McsRwExtendedBlock::kSuccFlagBusy) {
      new_next |= static_cast<uint64_t>(McsRwExtendedBlock::kSuccFlagBusy);
    }
    if (!pred_block->cas_next_weak(expected, new_next)) {
      goto retry;
    }

    return kErrorCodeLockCancelled;
  }

  ErrorCode cancel_writer_lock_no_pred(
    McsRwLock* lock, McsRwExtendedBlock* my_block, uint32_t my_tail_int) {
    spin_until([lock, my_block]{
      return lock->get_next_writer() != xct::McsRwLock::kNextWriterNone ||
        !my_block->pred_flag_is_waiting(); });
    if (my_block->pred_flag_is_granted() ||
      !lock->cas_next_writer_weak(adaptor_.get_my_id(), xct::McsRwLock::kNextWriterNone)) {
      // reader picked me up...
      spin_until([my_block]{ return my_block->pred_flag_is_granted(); });
      my_block->set_next_flag_granted();
      return kErrorCodeOk;
    }

    // so lock.next_writer is null now, try to fix the lock tail
    if (my_block->get_next_id() == 0 && lock->cas_tail_weak(my_tail_int, 0)) {
      return kErrorCodeLockCancelled;
    }

    spin_until([my_block]{ return my_block->get_next_id() != 0; });
    auto next_id = my_block->get_next_id();
    ASSERT_ND(next_id != McsRwExtendedBlock::kSuccIdSuccessorLeaving);

    // because I don't have a pred, if next_id is a writer, I should put it in lock.nw
    auto* succ_block = adaptor_.dereference_rw_tail_block(next_id);
    ASSERT_ND(succ_block->pred_flag_is_waiting());
    if (succ_block->is_writer()) {
      ASSERT_ND(my_block->next_flag_has_writer_successor());
      ASSERT_ND(lock->get_next_writer() == xct::McsRwLock::kNextWriterNone);
      // remaining readers will use CAS on lock.nw, so we blind write
      lock->set_next_writer(next_id >> 16);  // thread id only
      while (!succ_block->cas_pred_id_weak(my_tail_int, 0)) {}
    } else {
      // successor is a reader, lucky for it...
      ASSERT_ND(my_block->next_flag_has_reader_successor());
      ASSERT_ND(succ_block->is_reader());
      spin_until([succ_block, my_tail_int]{
        return succ_block->cas_pred_id_weak(my_tail_int, McsRwExtendedBlock::kPredIdAcquired); });
      lock->increment_nreaders();
      succ_block->set_pred_flag_granted();
    }
    return kErrorCodeLockCancelled;
  }

  ADAPTOR adaptor_;
};  // end of McsImpl<ADAPTOR, McsRwExtendedBlock> specialization


////////////////////////////////////////////////////////////////////////////////
/// Finally, explicit instantiation of the template class.
/// We instantiate the real adaptor for ThreadPimpl and the mock one for testing.
////////////////////////////////////////////////////////////////////////////////
template class McsWwImpl< McsMockAdaptor<McsRwSimpleBlock> >;
template class McsWwImpl< thread::ThreadPimplMcsAdaptor<McsRwSimpleBlock> >;

template class McsImpl< McsMockAdaptor<McsRwSimpleBlock> ,   McsRwSimpleBlock>;
template class McsImpl< McsMockAdaptor<McsRwExtendedBlock> , McsRwExtendedBlock>;
template class McsImpl< thread::ThreadPimplMcsAdaptor<McsRwSimpleBlock> ,   McsRwSimpleBlock>;
template class McsImpl< thread::ThreadPimplMcsAdaptor<McsRwExtendedBlock> , McsRwExtendedBlock>;
}  // namespace xct
}  // namespace foedus
