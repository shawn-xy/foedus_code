/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_SNAPSHOT_LOG_REDUCER_IMPL_HPP_
#define FOEDUS_SNAPSHOT_LOG_REDUCER_IMPL_HPP_
#include <foedus/epoch.hpp>
#include <foedus/fwd.hpp>
#include <foedus/initializable.hpp>
#include <foedus/log/fwd.hpp>
#include <foedus/log/log_id.hpp>
#include <foedus/memory/aligned_memory.hpp>
#include <foedus/snapshot/fwd.hpp>
#include <foedus/snapshot/mapreduce_base_impl.hpp>
#include <foedus/snapshot/snapshot_id.hpp>
#include <foedus/thread/fwd.hpp>
#include <stdint.h>
#include <iosfwd>
#include <string>
namespace foedus {
namespace snapshot {
/**
 * @brief A log reducer, which receives log entries sent from mappers
 * and applies them to construct new snapshot files.
 * @ingroup SNAPSHOT
 * @details
 * @section REDUCER_OVERVIEW Overview
 * Reducers receive log entries from mappers and apply them to new snapshot files.
 *
 * @section SORTING Sorting
 * The log entries are sorted by ordinal (*), then processed just like
 * usual APPLY at the end of transaction, but on top of snapshot files.
 *
 * (*) otherwise correct result is not guaranteed. For example, imagine the following case:
 *  \li UPDATE rec-1 to A. Log-ordinal 1.
 *  \li UPDATE rec-1 to B. Log-ordinal 2.
 * Ordinal-1 must be processed before ordinal 2.
 * As log entries are somewhat sorted already (due to how we write log files and buffer them in
 * mapper), we prefer bubble sort here. We so far use std::sort, though.
 *
 * @section DATAPAGES Data Pages
 * One tricky thing in reducer is how it manages data pages to read previous snapshot pages
 * and apply the new logs. So far, we assume each reducer allocates a sufficient amount of
 * DRAM to hold all pages it read/write during one snapshotting.
 * If this doesn't hold, we might directly allocate pages on NVRAM and read/write there.
 *
 * @note
 * This is a private implementation-details of \ref SNAPSHOT, thus file name ends with _impl.
 * Do not include this header from a client program. There is no case client program needs to
 * access this internal class.
 */
class LogReducer final : public MapReduceBase {
 public:
    LogReducer(Engine* engine, LogGleaner* parent, PartitionId id, thread::ThreadGroupId numa_node)
        : MapReduceBase(engine, parent, id, numa_node) {}

    /** One LogReducer corresponds to one snapshot partition. */
    PartitionId             get_id() const { return id_; }
    std::string             to_string() const override {
        return std::string("LogReducer-") + std::to_string(id_);
    }
    friend std::ostream&    operator<<(std::ostream& o, const LogReducer& v);

 protected:
    ErrorStack  handle_initialize() override;
    ErrorStack  handle_uninitialize() override;
    ErrorStack  handle_process() override;

 private:
    /**
     * memory to store all log entries in the epoch.
     * So far, this buffer has to contain all log entries in an epoch to the partition.
     * We have a few plans to alter the initial implementation.
     */
    memory::AlignedMemory   buffer_;
};
}  // namespace snapshot
}  // namespace foedus
#endif  // FOEDUS_SNAPSHOT_LOG_REDUCER_IMPL_HPP_