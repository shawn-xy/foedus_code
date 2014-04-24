/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_LOG_LOG_OPTIONS_HPP_
#define FOEDUS_LOG_LOG_OPTIONS_HPP_
#include <foedus/cxx11.hpp>
#include <foedus/externalize/externalizable.hpp>
#include <foedus/fs/device_emulation_options.hpp>
#include <foedus/log/log_id.hpp>
#include <stdint.h>
#include <string>
#include <vector>
namespace foedus {
namespace log {
/**
 * @brief Set of options for log manager.
 * @ingroup LOG
 * @details
 * This is a POD struct. Default destructor/copy-constructor/assignment operator work fine.
 */
struct LogOptions CXX11_FINAL : public virtual externalize::Externalizable {
    /** Constant values. */
    enum Constants {
        /** Default value for thread_buffer_kb_. */
        DEFAULT_THREAD_BUFFER_KB = (1 << 14),
        /** Default value for logger_buffer_kb_. */
        DEFAULT_LOGGER_BUFFER_KB = (1 << 14),
    };
    /**
     * Constructs option values with default values.
     */
    LogOptions();

    /**
     * @brief Full paths of log files.
     * @details
     * The files may or may not be on different physical devices.
     * This option also determines the number of loggers.
     * For the best performance, the number of loggers must be multiply of the number of NUMA
     * node and also be a submultiple of the total number of cores.
     * This is to evenly assign cores to loggers, loggers to NUMA nodes.
     * @attention The default value is just one entry of "foedus.log". When you modify this
     * setting, do NOT forget removing the default entry; call log_paths_.clear() first.
     */
    std::vector<std::string>    log_paths_;

    /** Size in KB of log buffer for \e each worker thread. */
    uint32_t                    thread_buffer_kb_;

    /** Size in KB of logger for \e each logger. */
    uint32_t                    logger_buffer_kb_;

    /** Settings to emulate slower logging device. */
    foedus::fs::DeviceEmulationOptions emulation_;

    EXTERNALIZABLE(LogOptions);

    /** Synonym for log_paths_.size(). */
    LoggerId                    get_logger_count() const { return log_paths_.size(); }
};
}  // namespace log
}  // namespace foedus
#endif  // FOEDUS_LOG_LOG_OPTIONS_HPP_
