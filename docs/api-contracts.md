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