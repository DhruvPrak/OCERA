#pragma once
#include <cstdint>
#include "common/page.h"

// Small shared types for the transaction / concurrency module.
namespace ocera {

using txn_id_t = std::uint64_t;
constexpr txn_id_t INVALID_TXN_ID = 0;  // real transaction IDs start at 1

// What a lock protects. PROPOSED decision P-03: page-level locking, so a
// resource is just a page id. If the team later picks table-level locks,
// only this alias (and its callers) need to change.
using ResourceId = page_id_t;

enum class LockMode { Shared, Exclusive };

}  // namespace ocera
