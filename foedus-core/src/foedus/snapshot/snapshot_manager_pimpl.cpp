/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include "foedus/snapshot/snapshot_manager_pimpl.hpp"

#include <glog/logging.h>

#include <chrono>
#include <string>

#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/error_stack_batch.hpp"
#include "foedus/assorted/atomic_fences.hpp"
#include "foedus/debugging/stop_watch.hpp"
#include "foedus/fs/filesystem.hpp"
#include "foedus/fs/path.hpp"
#include "foedus/log/log_manager.hpp"
#include "foedus/snapshot/log_gleaner_impl.hpp"
#include "foedus/snapshot/snapshot_metadata.hpp"
#include "foedus/snapshot/snapshot_options.hpp"
#include "foedus/storage/metadata.hpp"
#include "foedus/storage/storage_manager.hpp"

namespace foedus {
namespace snapshot {
const SnapshotOptions& SnapshotManagerPimpl::get_option() const {
  return engine_->get_options().snapshot_;
}

ErrorStack SnapshotManagerPimpl::initialize_once() {
  LOG(INFO) << "Initializing SnapshotManager..";
  if (!engine_->get_log_manager().is_initialized()) {
    return ERROR_STACK(kErrorCodeDepedentModuleUnavailableInit);
  }
  snapshot_epoch_.store(Epoch::kEpochInvalid);
  // TODO(Hideaki): get snapshot status from savepoint
  previous_snapshot_id_ = kNullSnapshotId;
  immediate_snapshot_requested_.store(false);
  previous_snapshot_time_ = std::chrono::system_clock::now();
  snapshot_thread_.initialize("Snapshot",
          std::thread(&SnapshotManagerPimpl::handle_snapshot, this),
          std::chrono::milliseconds(100));
  return kRetOk;
}

ErrorStack SnapshotManagerPimpl::uninitialize_once() {
  LOG(INFO) << "Uninitializing SnapshotManager..";
  ErrorStackBatch batch;
  if (!engine_->get_log_manager().is_initialized()) {
    batch.emprace_back(ERROR_STACK(kErrorCodeDepedentModuleUnavailableUninit));
  }
  snapshot_thread_.stop();
  return SUMMARIZE_ERROR_BATCH(batch);
}

void SnapshotManagerPimpl::handle_snapshot() {
  LOG(INFO) << "Snapshot thread started";
  // The actual snapshotting can't start until all other modules are initialized.
  SPINLOCK_WHILE(!snapshot_thread_.is_stop_requested() && !engine_->is_initialized()) {
    assorted::memory_fence_acquire();
  }

  LOG(INFO) << "Snapshot thread now starts taking snapshot";
  while (!snapshot_thread_.sleep()) {
    // should we start snapshotting? or keep sleeping?
    bool triggered = false;
    std::chrono::system_clock::time_point until = previous_snapshot_time_ +
      std::chrono::milliseconds(get_option().snapshot_interval_milliseconds_);
    Epoch durable_epoch = engine_->get_log_manager().get_durable_global_epoch();
    Epoch previous_epoch = get_snapshot_epoch();
    if (previous_epoch.is_valid() && previous_epoch == durable_epoch) {
      LOG(INFO) << "Current snapshot is already latest. durable_epoch=" << durable_epoch;
    } else if (immediate_snapshot_requested_) {
      // if someone requested immediate snapshot, do it.
      triggered = true;
      immediate_snapshot_requested_.store(false);
      LOG(INFO) << "Immediate snapshot request detected. snapshotting..";
    } else if (std::chrono::system_clock::now() >= until) {
      triggered = true;
      LOG(INFO) << "Snapshot interval has elapsed. snapshotting..";
    } else {
      // TODO(Hideaki): check free pages in page pool and compare with configuration.
    }

    if (triggered) {
      Snapshot new_snapshot;
      // TODO(Hideaki): graceful error handling
      COERCE_ERROR(handle_snapshot_triggered(&new_snapshot));
      Epoch new_snapshot_epoch = new_snapshot.valid_until_epoch_;
      ASSERT_ND(new_snapshot_epoch.is_valid() &&
        (!get_snapshot_epoch().is_valid() || new_snapshot_epoch > get_snapshot_epoch()));

      // done. notify waiters if exist
      Epoch::EpochInteger epoch_after = new_snapshot_epoch.value();
      previous_snapshot_id_ = new_snapshot.id_;
      previous_snapshot_time_ = std::chrono::system_clock::now();
      snapshot_taken_.notify_all([this, epoch_after]{ snapshot_epoch_.store(epoch_after); });
    } else {
      VLOG(1) << "Snapshotting not triggered. going to sleep again";
    }
  }

  LOG(INFO) << "Snapshot thread ended. ";
}

void SnapshotManagerPimpl::trigger_snapshot_immediate(bool wait_completion) {
  LOG(INFO) << "Requesting to immediately take a snapshot...";
  Epoch before = get_snapshot_epoch();
  Epoch durable_epoch = engine_->get_log_manager().get_durable_global_epoch();
  if (before.is_valid() && before == durable_epoch) {
    LOG(INFO) << "Current snapshot is already latest. durable_epoch=" << durable_epoch;
    return;
  }

  immediate_snapshot_requested_.store(true);
  snapshot_thread_.wakeup();
  if (wait_completion) {
    LOG(INFO) << "Waiting for the completion of snapshot... before=" << before;
    snapshot_taken_.wait([this, before]{ return before != get_snapshot_epoch(); });
    LOG(INFO) << "Observed the completion of snapshot! after=" << get_snapshot_epoch();
  }
}

ErrorStack SnapshotManagerPimpl::handle_snapshot_triggered(Snapshot *new_snapshot) {
  Epoch durable_epoch = engine_->get_log_manager().get_durable_global_epoch();
  Epoch previous_epoch = get_snapshot_epoch();
  LOG(INFO) << "Taking a new snapshot. durable_epoch=" << durable_epoch
    << ". previous_snapshot=" << previous_epoch;
  ASSERT_ND(durable_epoch.is_valid() &&
    (!previous_epoch.is_valid() || durable_epoch > previous_epoch));
  new_snapshot->base_epoch_ = previous_epoch;
  new_snapshot->valid_until_epoch_ = durable_epoch;

  // determine the snapshot ID
  SnapshotId snapshot_id;
  if (previous_snapshot_id_ == kNullSnapshotId) {
    snapshot_id = 1;
  } else {
    snapshot_id = increment(previous_snapshot_id_);
  }
  LOG(INFO) << "Issued ID for this snapshot:" << snapshot_id;
  new_snapshot->id_ = snapshot_id;

  // okay, let's start the snapshotting.
  // The procedures below will take long time, so we keep checking our "is_stop_requested"
  // and stops our child threads when it happens.

  // First, we determine partitioning policy for each storage so that we can scatter-gather
  // logs to each partition.

  // Second, we initiate log gleaners that do scatter-gather and consume the logs.
  // This will create snapshot files at each partition and tell us the new root pages of
  // each storage.
  CHECK_ERROR(glean_logs(new_snapshot));

  // Finally, write out the metadata file.
  CHECK_ERROR(snapshot_metadata(new_snapshot));

  return kRetOk;
}

ErrorStack SnapshotManagerPimpl::glean_logs(Snapshot* new_snapshot) {
  // Log gleaner is an object allocated/deallocated per snapshotting.
  // Make sure we call uninitialize even when there occurs an error.
  LogGleaner gleaner(engine_, new_snapshot, &snapshot_thread_);
  CHECK_ERROR(gleaner.initialize());
  ErrorStack result;
  {
    UninitializeGuard guard(&gleaner);
    // Gleaner runs on this thread (snapshot_thread_), so also receives the thread object
    // to check for termination request.
    result = gleaner.execute();
    if (result.is_error()) {
      LOG(ERROR) << "Log Gleaner encountered either an error or early termination request";
    }
    // the output is list of pointers to new root pages
    new_snapshot->new_root_page_pointers_ = gleaner.get_new_root_page_pointers();
    CHECK_ERROR(gleaner.uninitialize());
  }
  return result;
}

ErrorStack SnapshotManagerPimpl::snapshot_metadata(Snapshot *new_snapshot) {
  /* TODO(Hideaki) During surgery
  // construct metadata object
  SnapshotMetadata metadata;
  metadata.id_ = new_snapshot->id_;
  metadata.base_epoch_ = new_snapshot->base_epoch_.value();
  metadata.valid_until_epoch_ = new_snapshot->valid_until_epoch_.value();
  CHECK_ERROR(engine_->get_storage_manager().clone_all_storage_metadata(&metadata));

  // we modified the root page. install it.
  uint32_t installed_root_pages_count = 0;
  for (storage::Metadata* meta : metadata.storage_metadata_) {
    const auto& it = new_snapshot->new_root_page_pointers_.find(meta->id_);
    if (it != new_snapshot->new_root_page_pointers_.end()) {
      storage::SnapshotPagePointer new_pointer = it->second;
      ASSERT_ND(new_pointer != meta->root_snapshot_page_id_);
      meta->root_snapshot_page_id_ = new_pointer;
      ++installed_root_pages_count;
    }
  }
  LOG(INFO) << "Out of " << metadata.storage_metadata_.size() << " storages, "
    << installed_root_pages_count << " changed their root pages.";
  ASSERT_ND(installed_root_pages_count == new_snapshot->new_root_page_pointers_.size());

  // save it to a file
  fs::Path folder(get_option().get_primary_folder_path());
  if (!fs::exists(folder)) {
    if (!fs::create_directories(folder, true)) {
      LOG(ERROR) << "Failed to create directory:" << folder << ". check permission.";
      return ERROR_STACK(kErrorCodeFsMkdirFailed);
    }
  }

  fs::Path file = get_snapshot_metadata_file_path(new_snapshot->id_);
  LOG(INFO) << "New snapshot metadata file fullpath=" << file;

  debugging::StopWatch stop_watch;
  CHECK_ERROR(metadata.save_to_file(file));
  stop_watch.stop();
  LOG(INFO) << "Wrote a snapshot metadata file. size=" << fs::file_size(file) << " bytes"
    << ", elapsed time to write=" << stop_watch.elapsed_ms() << "ms. now fsyncing...";
  stop_watch.start();
  fs::fsync(file, true);
  stop_watch.stop();
  LOG(INFO) << "fsynced the file and the folder! elapsed=" << stop_watch.elapsed_ms() << "ms.";
  */
  return kRetOk;
}

fs::Path SnapshotManagerPimpl::get_snapshot_metadata_file_path(SnapshotId snapshot_id) const {
  fs::Path folder(get_option().get_primary_folder_path());
  fs::Path file(folder);
  file /= std::string("snapshot_metadata_")
    + std::to_string(snapshot_id) + std::string(".xml");
  return file;
}

}  // namespace snapshot
}  // namespace foedus
