/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_STORAGE_HASH_FWD_HPP_
#define FOEDUS_STORAGE_HASH_FWD_HPP_
/**
 * @file foedus/storage/hash/fwd.hpp
 * @brief Forward declarations of classes in hash storage package.
 * @ingroup HASH
 */
namespace foedus {
namespace storage {
namespace hash {
struct  HashCreateLogType;
struct  HashOverwriteLogType;
struct  HashMetadata;
class   HashPage;
class   HashPartitioner;
class   HashStorage;
class   HashStorageFactory;
class   HashStoragePimpl;
}  // namespace hash
}  // namespace storage
}  // namespace foedus
#endif  // FOEDUS_STORAGE_HASH_FWD_HPP_