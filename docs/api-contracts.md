# OCERA — API Contracts Between Modules

This document records the public interfaces currently implemented in OCERA and identifies planned modules whose APIs are not yet implemented.

**Rule:** Keep this document synchronized with the actual source code. Do not describe a planned interface as implemented or an implementation detail as a team-approved architectural decision.

## 1. Shared Types and Constants

Headers: `src/common/config.h`, `src/common/page.h`

| Symbol | Type / Value | Description |
|---|---|---|
| `PAGE_SIZE` | `std::size_t`, `4096` | Page size in bytes |
| `BITMAP_PAGE_ID` | `std::int32_t`, `0` | Reserved free-space bitmap page |
| `HEADER_PAGE_ID` | `std::int32_t`, `1` | Reserved database header page |
| `page_id_t` | `int32_t` | Page identifier |
| `INVALID_PAGE_ID` | `page_id_t`, `-1` | Invalid page identifier |
| `Page` | Structure containing `char data[PAGE_SIZE]` | Fixed-size page buffer |

Header: `src/txn/txn_types.h`

| Symbol | Type / Value | Description |
|---|---|---|
| `txn_id_t` | `std::uint64_t` | Transaction identifier |
| `INVALID_TXN_ID` | `txn_id_t`, `0` | Invalid transaction identifier |
| `ResourceId` | Alias for `page_id_t` | Resource identifier used by the lock manager |
| `LockMode` | `enum class { Shared, Exclusive }` | Lock modes |

Transaction IDs start at `1` and increase as transactions are created. The current deadlock victim policy treats the highest transaction ID as the youngest transaction.

## 2. Storage APIs

### 2.1 DiskManager

Header: `src/storage/disk_manager.h`

| API | Contract |
|---|---|
| `explicit DiskManager(const std::string& db_file)` | Opens or creates the database file. |
| `~DiskManager()` | Releases resources owned by the manager. |
| `void ReadPage(page_id_t page_id, char* page_data)` | Reads `PAGE_SIZE` bytes into the supplied buffer. Unwritten pages are returned as zero-filled buffers. |
| `void WritePage(page_id_t page_id, const char* page_data)` | Writes `PAGE_SIZE` bytes and grows the file when necessary. |
| `page_id_t GetNumPages() const` | Returns the number of pages currently backed by the file. |

**Notes:**
- `DiskManager` is non-copyable.
- Callers must provide buffers large enough for `PAGE_SIZE` bytes.
- The implementation uses file I/O and a mutex to protect database-file operations.
- Verify the implementation before documenting exact exception behavior.

### 2.2 FreeSpaceManager

Header: `src/storage/free_space_manager.h`

| API | Contract |
|---|---|
| `explicit FreeSpaceManager(DiskManager& disk_manager)` | Initializes the free-space manager using the supplied disk manager. |
| `page_id_t AllocatePage()` | Allocates a free page and returns its ID. Throws `std::runtime_error` when the bitmap is full. |
| `void DeallocatePage(page_id_t page_id)` | Marks a page as free for future reuse. |
| `bool IsAllocated(page_id_t page_id) const` | Reports whether a page is marked as allocated. |

**Current design and limitations:**
- Page `0` stores the bitmap.
- Page `1` is reserved for the database header.
- A single page-sized bitmap can represent at most `PAGE_SIZE * 8` page IDs.
- Invalid and out-of-range page ID validation should be reviewed in the implementation.
- `FreeSpaceManager` manages page allocation; a separate `PageManager` API is not currently documented as implemented.

### 2.3 DatabaseHeader

Header: `src/storage/database_header.h`

| API / Symbol | Contract |
|---|---|
| `static constexpr char kMagic[4] = {'M', 'D', 'B', '1'}` | Identifies the current database file format. |
| `static constexpr std::uint32_t kCurrentVersion = 1` | Current file-format version. |
| `explicit DatabaseHeader(DiskManager& disk_manager)` | Initializes a new header or loads and validates an existing header. |
| `std::uint32_t GetVersion() const` | Returns the stored format version. |
| `std::uint32_t GetPageSize() const` | Returns the stored page size. |
| `page_id_t GetCatalogRootPageId() const` | Returns the catalog root page ID. |

**Notes:**
- Header fields are serialized at fixed offsets instead of writing a raw C++ structure.
- Construct `FreeSpaceManager` before `DatabaseHeader` so the reserved pages are initialized consistently.
- The header stores format metadata; it does not provide crash recovery by itself.

## 3. Transaction and Concurrency APIs

### 3.1 LockManager

Header: `src/txn/lock_manager.h`

| API | Contract |
|---|---|
| `bool LockShared(txn_id_t txn_id, ResourceId resource)` | Requests a shared lock. Waits when blocked and returns `false` if the transaction is aborted while waiting. |
| `bool LockExclusive(txn_id_t txn_id, ResourceId resource)` | Requests an exclusive lock. Waits when blocked and returns `false` if the transaction is aborted while waiting. |
| `void ReleaseAllLocks(txn_id_t txn_id)` | Removes pending requests, releases held locks, and wakes waiting transactions. |
| `void AbortTransaction(txn_id_t txn_id)` | Marks a transaction as aborted for lock-request purposes and wakes affected waiters. |
| `std::vector<std::pair<txn_id_t, txn_id_t>> GetWaitForEdges() const` | Returns wait-for edges represented as `(waiter, holder)` pairs. |
| `bool HoldsLock(txn_id_t txn_id, ResourceId resource, LockMode mode) const` | Checks whether the transaction holds the requested lock mode. |
| `std::size_t NumLocksHeld(txn_id_t txn_id) const` | Returns the number of resources for which the transaction holds locks. |

**Current behavior:**
- A mutex protects the lock table.
- Requests are queued per resource.
- Waiting requests are served in FIFO order.
- Lock upgrades receive priority in the waiting queue.
- Re-requesting a lock already held, or requesting a weaker mode, returns `true`.
- An aborted transaction's lock requests fail until its locks and pending requests are cleaned up.
- `ResourceId` currently aliases `page_id_t`, so resources are identified at page level.

Page-level locking and Strict Two-Phase Locking are implemented design choices whose formal team approval must be recorded separately.

### 3.2 TransactionManager

Header: `src/txn/transaction_manager.h`

Transaction states:

`TransactionState::Active`, `TransactionState::Committed`, and `TransactionState::Aborted`.

| API | Contract |
|---|---|
| `explicit TransactionManager(LockManager& lock_manager)` | Creates a transaction manager using the supplied lock manager. |
| `txn_id_t Begin()` | Creates a new active transaction with a unique increasing ID. |
| `bool Commit(txn_id_t txn_id)` | Commits an active transaction and releases its locks. Returns `false` for an unknown or already-finished transaction. |
| `bool Abort(txn_id_t txn_id)` | Aborts an active transaction and releases its locks. Returns `false` for an unknown or already-finished transaction. |
| `bool Lock(txn_id_t txn_id, ResourceId resource, LockMode mode)` | Acquires a lock for an active transaction. Returns `false` if the transaction is inactive or the lock request fails. |
| `bool GetState(txn_id_t txn_id, TransactionState* state) const` | Returns `false` for an unknown transaction; otherwise writes its state and returns `true`. |

**Notes:**
- The caller of `GetState()` must supply a valid, non-null pointer.
- Transactions cannot acquire new locks after they finish.
- The transaction's worker thread should perform its own `Commit()` or `Abort()` cleanup after a deadlock victim's lock request fails.
- `Abort()` currently changes transaction state and releases locks. It does not undo modified database pages.
- Data rollback requires an implemented undo/recovery mechanism.

### 3.3 DeadlockDetector

Header: `src/txn/deadlock_detector.h`

| API | Contract |
|---|---|
| `using Edge = std::pair<txn_id_t, txn_id_t>` | Represents a wait-for edge `(waiter, holder)`. |
| `static std::vector<txn_id_t> FindCycle(const std::vector<Edge>& edges)` | Returns one detected cycle or an empty vector if no cycle exists. |
| `static txn_id_t ChooseVictim(const std::vector<txn_id_t>& cycle)` | Selects the highest transaction ID. Returns `INVALID_TXN_ID` for an empty cycle. |
| `std::size_t DetectAndResolve()` | Detects deadlocks and marks one victim per detected cycle. Returns the number of victims selected during the pass. |
| `void Start(std::chrono::milliseconds interval)` | Starts periodic background deadlock detection. |
| `void Stop()` | Stops and joins the detector thread; safe to call repeatedly. |
| `std::size_t DeadlocksResolved() const` | Returns the atomic count of victims selected by the detector. |

**Current behavior:**
- Deadlocks are detected using cycles in the wait-for graph.
- The highest transaction ID in a cycle is selected as the victim.
- The detector marks the victim through `LockManager::AbortTransaction()`.
- The affected transaction's worker is responsible for calling `TransactionManager::Abort()` and releasing its locks.

The victim-selection policy and transaction cleanup responsibilities should be recorded as confirmed architectural decisions only after explicit team agreement.

## 4. Planned Modules

The following modules are planned but do not have implemented public APIs in the reviewed source snapshot.

| Module | Responsible teammate | Planned responsibility |
|---|---|---|
| Buffer Pool Manager and LRU replacement | Dhruv | Page caching, pin/unpin operations, dirty-page tracking, eviction, and integration with disk storage |
| B+Tree indexing | Ishika | Index page layout, search, insertion, deletion, and integration with storage |
| SQL parser and query executor | Ishika | SQL parsing, query execution, and integration with the catalog and indexes |
| WAL, checkpointing, and crash recovery | Ishika | Logging, durability, recovery, and the future undo/redo mechanism |

These statuses must be updated when the corresponding code is implemented and integrated.

Final interfaces should be agreed upon by the relevant module owners before dependent modules are built against them.

## 5. Build Targets

The reviewed CMake configuration defines these executable targets:

- `ocera`
- `test_storage`
- `test_header`
- `test_lock_manager`
- `test_transaction_manager`
- `test_deadlock_detector`
- `demo_concurrency`

It also defines the `ocera_storage` and `ocera_txn` libraries.

The reviewed CMake configuration does not register these executables with CTest through `add_test()`.

## 6. API Change Procedure

When changing a public interface:

1. Update the header and implementation.
2. Update the corresponding tests.
3. Update this document.
4. Notify owners of dependent modules.
5. Verify the relevant build targets and tests.
6. Do not document planned functionality as implemented until it exists.

## Change Log

| Date | Change | Status |
|---|---|---|
| 2026-10-14 | Separated implemented interfaces from planned modules and documented current limitations | Review draft; team approval pending |
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

## 8. Lock Manager implemented; deadlock detection not yet verified — Bhavya

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

## 9. WAL and recovery — proposed; owner pending team confirmation

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
