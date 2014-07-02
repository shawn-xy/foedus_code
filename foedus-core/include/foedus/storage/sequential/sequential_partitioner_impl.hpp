/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_STORAGE_SEQUENTIAL_SEQUENTIAL_PARTITIONER_IMPL_HPP_
#define FOEDUS_STORAGE_SEQUENTIAL_SEQUENTIAL_PARTITIONER_IMPL_HPP_

#include <stdint.h>

#include <iosfwd>

#include "foedus/fwd.hpp"
#include "foedus/memory/fwd.hpp"
#include "foedus/storage/partitioner.hpp"
#include "foedus/storage/storage_id.hpp"
#include "foedus/storage/sequential/sequential_id.hpp"

namespace foedus {
namespace storage {
namespace sequential {
/**
 * @brief Partitioner for an sequential storage.
 * @ingroup SEQUENTIAL
 * @details
 * Partitioning/sorting policy for \ref SEQUENTIAL is super simple; it does nothing.
 * We put all logs in node-x to snapshot of node-x for the best performance.
 * As the only read access pattern is full-scan, we don't care partitioning.
 * We just minimize the communication cost by this policy.
 * No sorting either.
 *
 * @note
 * This is a private implementation-details of \ref SEQUENTIAL, thus file name ends with _impl.
 * Do not include this header from a client program. There is no case client program needs to
 * access this internal class.
 */
class SequentialPartitioner final : public virtual Partitioner {
 public:
  SequentialPartitioner(Engine *engine, StorageId id);
  ~SequentialPartitioner() {}
  StorageId get_storage_id() const override { return sequential_id_; }
  StorageType get_storage_type() const override { return kSequentialStorage; }
  Partitioner* clone() const override { return new SequentialPartitioner(*this); }
  void describe(std::ostream* o) const override;

  bool is_partitionable() const override { return true; }
  void partition_batch(
    PartitionId                     local_partition,
    const snapshot::LogBuffer&      log_buffer,
    const snapshot::BufferPosition* log_positions,
    uint32_t                        logs_count,
    PartitionId*                    results) const override;

  void sort_batch(
    const snapshot::LogBuffer&        log_buffer,
    const snapshot::BufferPosition*   log_positions,
    uint32_t                          logs_count,
    const memory::AlignedMemorySlice& sort_buffer,
    Epoch                             base_epoch,
    snapshot::BufferPosition*         output_buffer,
    uint32_t*                         written_count) const override;

  uint64_t  get_required_sort_buffer_size(uint32_t log_count) const override { return 0; }

 private:
  /** only for sanity check */
  StorageId             sequential_id_;
};
}  // namespace sequential
}  // namespace storage
}  // namespace foedus
#endif  // FOEDUS_STORAGE_SEQUENTIAL_SEQUENTIAL_PARTITIONER_IMPL_HPP_