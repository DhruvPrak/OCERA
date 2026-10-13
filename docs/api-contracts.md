# OCERA — API Contracts Between Modules

**Status:** existing storage APIs below are verified from source. Higher-layer APIs are **proposals for team approval**, not implemented code.

The purpose of this document is to let teammates implement modules independently without inventing incompatible function names or data formats. If an approved signature changes, update this file in the same PR.

## 1. Shared types and constants — existing

Source: `src/common/config.h`, `src/common/page.h`.

- Namespace: `ocera`
- `constexpr std::size_t PAGE_SIZE = 4096`
- `constexpr std::int32_t BITMAP_PAGE_ID = 0`
- `constexpr std::int32_t HEADER_PAGE_ID = 1`
- `using page_id_t = std::int32_t`
- `constexpr page_id_t INVALID_PAGE_ID = -1`
- `struct Page { char data[PAGE_SIZE]; ... }`; the constructor zero-initializes the byte array.

A `Page` is raw page bytes only. Do not put pin counts, dirty flags or LRU metadata inside the persisted page bytes; those belong in Buffer Pool frame metadata.

## 2. Storage layer — existing APIs

Sources: `src/storage/disk_manager.h`, `free_space_manager.h`, `database_header.h`.

### DiskManager

```cpp
explicit DiskManager(const std::string& db_file);
~DiskManager();
void ReadPage(page_id_t page_id, char* page_data);
void WritePage(page_id_t page_id, const char* page_data);
page_id_t GetNumPages() const;
```

- `ReadPage` fills exactly `PAGE_SIZE` bytes; an unwritten page reads as zeros.
- `WritePage` writes one fixed-size page and can grow the file.
- A DiskManager instance is non-copyable and protects file I/O with a mutex.
- Caller must supply a valid non-null buffer of at least `PAGE_SIZE` bytes. Invalid page IDs should not be passed.

### FreeSpaceManager

```cpp
explicit FreeSpaceManager(DiskManager& disk_manager);
page_id_t AllocatePage();
void DeallocatePage(page_id_t page_id);
bool IsAllocated(page_id_t page_id) const;
```

- Page 0 is the bitmap and page 1 is the database header; these are reserved.
- `AllocatePage` throws `std::runtime_error` if the bitmap is full.
- Current single-bitmap capacity: 32,768 page IDs (128 MiB at 4 KiB/page); bitmap chaining is future work.
- Callers must not deallocate reserved pages.

### DatabaseHeader

```cpp
explicit DatabaseHeader(DiskManager& disk_manager);
std::uint32_t GetVersion() const;
std::uint32_t GetPageSize() const;
page_id_t GetCatalogRootPageId() const;
```

- Lives at page ID 1; magic is currently `MDB1`; format version is 1.
- Header fields are serialized at explicit offsets, not by writing a raw C++ struct.
- The free-space manager must reserve the header page before it could be allocated to another caller.
- Catalog root page ID is metadata; the Catalog implementation itself is not yet present.

## 3. Buffer Pool — proposed API; Dhruv owns it

**Purpose:** cache pages in memory and keep buffer metadata separate from the on-disk page bytes.

Proposed minimal interface (names/signatures need team approval before implementation):

```cpp
class BufferPoolManager {
public:
    BufferPoolManager(std::size_t pool_size,
                      DiskManager& disk_manager,
                      FreeSpaceManager& free_space_manager);
    Page* FetchPage(page_id_t page_id);
    bool UnpinPage(page_id_t page_id, bool is_dirty);
    bool FlushPage(page_id_t page_id);
    void FlushAllPages();
    Page* NewPage(page_id_t* page_id);
    bool DeletePage(page_id_t page_id);
};
```

Contract proposal:
- A successful `FetchPage` returns a pinned page; the caller must call `UnpinPage` once when finished.
- `UnpinPage(id, true)` marks the frame dirty; dirty pages are written by flush/eviction.
- `FetchPage` returns `nullptr` if it cannot obtain a frame or load the page. `NewPage` returns `nullptr` if no frame is available and communicates the allocated ID through the output pointer on success.
- A pinned frame cannot be evicted.
- LRU replacement is planned, but record the algorithm as confirmed only after the team approves it.
- Thread safety, behavior on duplicate pin/unpin, and page deletion with active pins must be tested and agreed.

## 4. Record, table and Catalog layer — proposed; Ishika owns this foundation

These structures are not implemented yet. Agree the byte layout before writing pages to disk.

Proposed minimum scope:
- A stable record identifier, tentatively `RID { page_id_t page_id; std::uint32_t slot_num; }`.
- A table-page layout that can locate records and free space.
- A Catalog mapping table names/IDs to schema and root/page metadata.
- A simple typed value representation and schema definition shared by parser and executor.

The exact record encoding, supported types, slot layout, schema persistence and Catalog root semantics remain open. Do not serialize C++ structs directly to disk; specify field offsets/encoding.

## 5. B+Tree — proposed; Ishika owns it

Proposed logical operations:
- `Insert(key, RID)`
- `Search(key) -> optional<RID>` or a collection if duplicate keys are supported
- `Remove(key)`
- range scan only if time permits.

Open questions: key/value types, duplicate-key policy, node page layout, root-page updates, and how index changes are logged/recovered. Start with one documented key type if generic keys create unnecessary complexity.

## 6. SQL parser and execution — proposed; Ishika owns it

Proposed flow:

`SQL text -> parser/statement representation -> validation using Catalog -> executor -> Buffer Pool/table/index APIs`.

Minimum intended statements: single-table `CREATE TABLE` if needed for setup, `INSERT`, `SELECT`, `UPDATE`, `DELETE`. WHERE support should begin with simple comparisons and be explicitly documented. Joins and advanced SQL are stretch goals.

The parser should return a structured statement or a clear parse error; it should not directly read/write database pages. The executor should invoke transaction APIs and table/index APIs rather than bypassing them.

Exact AST types and public function signatures should be agreed when the supported grammar is chosen.

## 7. Transaction Manager — implemented; Bhavya owns it

**Status:** Implemented in `feature/concurrency`; transaction manager tests passed. The interface below is verified against `src/txn/transaction_manager.h`.

```cpp
txn_id_t Begin();
bool Commit(txn_id_t txn_id);
bool Abort(txn_id_t txn_id);
bool Lock(txn_id_t txn_id, ResourceId resource, LockMode mode);
bool GetState(txn_id_t txn_id, TransactionState* state) const;
```

- Transaction IDs increase from 1 and are unique within the running engine.
- `Commit()` and `Abort()` finish an active transaction and release its locks.
- `Lock()` delegates lock acquisition to the Lock Manager and fails if the transaction is inactive or has been marked as a deadlock victim.
- `GetState()` reports the transaction state when the ID is known.
- `Abort()` currently releases locks and records the aborted state; it does not yet undo data changes.

**Pending:** WAL/undo integration, durable rollback, and recovery testing remain future work. The proposed interface must not be presented as a substitute for the verified C++ interface.

## 8. Lock Manager and deadlock detection — implemented; Bhavya owns it

**Status:** Implemented in `feature/concurrency`; lock manager tests passed. The interface below is verified against `src/txn/lock_manager.h`.

```cpp
bool LockShared(txn_id_t txn_id, ResourceId resource);
bool LockExclusive(txn_id_t txn_id, ResourceId resource);
void ReleaseAllLocks(txn_id_t txn_id);
void AbortTransaction(txn_id_t txn_id);
std::vector<std::pair<txn_id_t, txn_id_t>> GetWaitForEdges() const;
```

- Locks are page-resource based in the current implementation.
- Shared locks can coexist; exclusive locks conflict with other transactions' locks.
- Conflicting requests wait until they can be granted or the transaction is marked as a deadlock victim.
- `ReleaseAllLocks()` releases all locks held by a transaction and wakes waiting requests.
- `AbortTransaction()` marks a transaction as aborted for lock acquisition; the transaction's own thread must then call `Abort()`.
- `GetWaitForEdges()` provides a snapshot of the wait-for graph for deadlock detection.
- Strict 2PL behavior is implemented: locks are released together at transaction completion, not incrementally.

**Important:** The Lock Manager exposes the wait-for graph, but this does not by itself mean deadlock detection and recovery are fully integrated with WAL or durable undo/redo.

## 9. WAL and recovery — proposed; Ishika owns it, with Bhavya/Dhruv integration

Proposed responsibilities:
- Append log records with a monotonically increasing log sequence number (LSN).
- Enforce write-ahead rule: the relevant log record must reach durable storage before the corresponding dirty data page is flushed.
- Provide commit durability and startup recovery hooks.
- Support a documented checkpoint mechanism.

Exact interface, record format, physical/logical logging, page LSN storage, undo/redo strategy, fsync policy and checkpoint algorithm remain open. Agree these before the Buffer Pool starts flushing pages in ways that assume WAL semantics.

## 10. Dependency and call direction

```
SQL Parser -> Validator/Catalog -> SQL Executor
                                  |
                                  v
                         Transaction Manager
                                  |
                                  v
                             Lock Manager

SQL Executor / B+Tree / Table Pages
                 |
                 v
          Buffer Pool Manager
                 |
                 v
             DiskManager
                 |
                 v
          Database file on disk

Transaction Manager <-> WAL/Recovery coordination
Buffer Pool Manager <-> WAL flush-order coordination
Catalog / B+Tree -> page IDs and page bytes via Buffer Pool
```

Keep dependencies flowing toward lower-level services. DiskManager should not depend on SQL, B+Tree, transactions or the Buffer Pool. SQL should not call raw DiskManager I/O directly. Avoid circular header dependencies; use small shared types in `src/common/`.

## 11. Team approval checklist

Before parallel implementation, agree and record:
- [ ] Buffer Pool API and pin/unpin/eviction semantics
- [ ] Record/table-page layout and Catalog schema representation
- [ ] B+Tree key type and duplicate-key policy
- [ ] Supported SQL grammar and result/error representation
- [ ] Lock granularity, blocking behavior, 2PL variant and deadlock victim policy
- [ ] WAL record format, redo/undo scope and required flush ordering
- [ ] Who updates which shared headers and how API changes are reviewed

Until checked, the proposed contracts are guidance for discussion and mock-based tests—not authorization to silently change existing code.
