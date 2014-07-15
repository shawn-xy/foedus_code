/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_STORAGE_HASH_HASH_STORAGE_PIMPL_HPP_
#define FOEDUS_STORAGE_HASH_HASH_STORAGE_PIMPL_HPP_
#include <stdint.h>

#include <string>
#include <vector>

#include "foedus/compiler.hpp"
#include "foedus/cxx11.hpp"
#include "foedus/fwd.hpp"
#include "foedus/initializable.hpp"
#include "foedus/assorted/const_div.hpp"
#include "foedus/memory/fwd.hpp"
#include "foedus/storage/fwd.hpp"
#include "foedus/storage/storage.hpp"
#include "foedus/storage/storage_id.hpp"
#include "foedus/storage/hash/fwd.hpp"
#include "foedus/storage/hash/hash_id.hpp"
#include "foedus/storage/hash/hash_metadata.hpp"
#include "foedus/thread/fwd.hpp"

namespace foedus {
namespace storage {
namespace hash {

/**
* @brief Pimpl object of HashStorage.
 * @ingroup HASH
 * @details
 * A private pimpl object for HashStorage.
 * Do not include this header from a client program unless you know what you are doing.
 */
class HashStoragePimpl final : public DefaultInitializable {
 public:
  HashStoragePimpl() = delete;
  HashStoragePimpl(Engine* engine, HashStorage* holder, const HashMetadata &metadata,
            bool create);

  ErrorStack  initialize_once() override;
  ErrorStack  uninitialize_once() override;

  /** Used only from uninitialize() */
  void        release_pages_recursive_root(
    memory::PageReleaseBatch* batch,
    HashRootPage* page,
    VolatilePagePointer volatile_page_id);
  void        release_pages_recursive_bin(
    memory::PageReleaseBatch* batch,
    HashBinPage* page,
    VolatilePagePointer volatile_page_id);
  void        release_pages_recursive_data(
    memory::PageReleaseBatch* batch,
    HashDataPage* page,
    VolatilePagePointer volatile_page_id);

  ErrorStack  create(thread::Thread* context);

  /** @copydoc foedus::storage::hash::HashStorage::get_record() */
  ErrorCode   get_record(
    thread::Thread* context,
    const void* key,
    uint16_t key_length,
    void* payload,
    uint16_t* payload_capacity);

  /** @copydoc foedus::storage::hash::HashStorage::get_record_part() */
  ErrorCode   get_record_part(
    thread::Thread* context,
    const char* key,
    uint16_t key_length,
    void* payload,
    uint16_t payload_offset,
    uint16_t payload_count);

  /** @copydoc foedus::storage::hash::HashStorage::insert_record() */
  ErrorCode insert_record(
    thread::Thread* context,
    const char* key,
    uint16_t key_length,
    const void* payload,
    uint16_t payload_count);

  /** @copydoc foedus::storage::hash::HashStorage::delete_record() */
  ErrorCode delete_record(thread::Thread* context, const char* key, uint16_t key_length);

  /** @copydoc foedus::storage::hash::HashStorage::overwrite_record() */
  ErrorCode overwrite_record(
    thread::Thread* context,
    const char* key,
    uint16_t key_length,
    const void* payload,
    uint16_t payload_offset,
    uint16_t payload_count);

  /**
   * @brief Find a bin page that contains a bin for the hash.
   * @param[in] context Thread context
   * @param[in,out] combo Hash values. Also the result of this method.
   * @details
   * It might set null to out if there is no bin page created yet.
   * In this case, the pointer to the bin page is added as a node set to capture a concurrent
   * event installing a new volatile page there.
   */
  ErrorCode     lookup_bin(thread::Thread* context, HashCombo *combo);

  HashRootPage* lookup_boundary_root(
    thread::Thread* context,
    uint64_t bin,
    uint16_t *pointer_index) ALWAYS_INLINE;

  ErrorCode     locate_record(
    thread::Thread* context,
    const void* key,
    uint16_t key_length,
    HashCombo* combo) ALWAYS_INLINE;

  Engine* const           engine_;
  HashStorage* const      holder_;
  HashMetadata            metadata_;

  /**
   * Points to the root page.
   */
  DualPagePointer         root_page_pointer_;

  /**
   * Root page is assured to be never evicted.
   * So, we can access the root_page_ without going through caching module.
   */
  HashRootPage*           root_page_;

  bool                    exist_;
  uint32_t                root_pages_;
  uint64_t                bin_count_;
  uint64_t                bin_pages_;
};
}  // namespace hash
}  // namespace storage
}  // namespace foedus
#endif  // FOEDUS_STORAGE_HASH_HASH_STORAGE_PIMPL_HPP_