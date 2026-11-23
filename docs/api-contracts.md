# OCERA — API Contracts Between Modules

This document describes the public interfaces present on `feature/wal-recovery` and separates implemented functionality from proposals and missing integration work.

**Source of truth:** the headers, implementations, CMake configuration, and tests in the repository. A proposal in this document is not an implemented API or an approved architectural decision.

## 1. Shared types and constants

Headers: `src/common/config.h`, `src/common/page.h`, `src/common/rid.h`

| Symbol | Current definition / contract |
|---|---|
| `PAGE_SIZE` | `std::size_t`, 4096 bytes |
| `BITMAP_PAGE_ID` | `std::int32_t`, 0; reserved free-space bitmap page |
| `HEADER_PAGE_ID` | `std::int32_t`, 1; reserved database header page |
| `page_id_t` | `std::int32_t` |
| `INVALID_PAGE_ID` | `page_id_t`, -1 |
| `Page` | Fixed-size page buffer containing `char data[PAGE_SIZE]` |
| `RID` | `{ page_id_t page_id; std::uint32_t slot_num; std::uint32_t generation; }` |
| `INVALID_TXN_ID` | `txn_id_t`, 0 |

`RID` equality compares all three fields. The generation distinguishes a newly inserted record from an older record that occupied the same slot.

Transaction types are declared in `src/txn/txn_types.h`:

- `txn_id_t` is `std::uint64_t`.
- `ResourceId` aliases `page_id_t`.
- `LockMode` has `Shared` and `Exclusive` values.

## 2. Storage APIs

### 2.1 DiskManager

Header: `src/storage/disk_manager.h`

```cpp
explicit DiskManager(
    const std::string& db_file,
    DatabaseOpenMode mode = DatabaseOpenMode::CREATE_OR_OPEN);
~DiskManager();

void ReadPage(page_id_t page_id, char* page_data);
void WritePage(page_id_t page_id, const char* page_data);
void Sync();
page_id_t GetNumPages() const;
```

- `DatabaseOpenMode` supports `CREATE_NEW`, `OPEN_EXISTING`, and `CREATE_OR_OPEN`.
- Reads and writes one `PAGE_SIZE` page; unwritten pages are zero-filled.
- `WritePage()` flushes the C++ stream. `Sync()` additionally requests OS-level durable synchronization.
- DiskManager remains the raw file-I/O boundary; higher layers enforce WAL ordering before persistent page writes.

### 2.2 FreeSpaceManager

Header: `src/storage/free_space_manager.h`

```cpp
explicit FreeSpaceManager(DiskManager& disk_manager,
                          LogManager* log_manager = nullptr);

page_id_t AllocatePage();
void DeallocatePage(page_id_t page_id);
bool IsAllocated(page_id_t page_id) const;
```

- Page 0 stores the allocation bitmap; page 1 is reserved for the database header.
- A single page-sized bitmap represents at most `PAGE_SIZE * 8` page IDs.
- When supplied a LogManager, bitmap changes are logged as redo-only system updates and the log is synchronized before page 0 is written.
- `AllocatePage()` throws `std::runtime_error` when the bitmap is full. Deallocation rejects reserved, out-of-range, and already-free page IDs.

### 2.3 DatabaseHeader

Header: `src/storage/database_header.h`

Current format constants:

```cpp
static constexpr char kMagic[4] = {'M', 'D', 'B', '1'};
static constexpr std::uint32_t kCurrentVersion = 2;
```

Public interface:

```cpp
explicit DatabaseHeader(DiskManager& disk_manager,
                        LogManager* log_manager = nullptr);

std::uint32_t GetVersion() const;
std::uint32_t GetPageSize() const;
page_id_t GetCatalogRootPageId() const;
const DatabaseId& GetDatabaseId() const;
void SetCatalogRootPageId(page_id_t page_id);
```

- Page 1 stores explicit little-endian fields, a 128-bit database identity, and a CRC.
- `Bootstrap()` runs before the WAL is opened. It initializes a new database or performs a journaled v1-to-v2 migration.
- `CompleteBootstrap()` retires the migration journal only after the matching WAL opens.
- Post-bootstrap header updates are redo-only WAL page updates. If the header is damaged after a logged update, the valid WAL header identity can be used to open the WAL and replay page 1 before normal header validation.

### 2.4 BufferPoolManager

Header: `src/storage/buffer_pool_manager.h`

```cpp
BufferPoolManager(std::size_t pool_size,
                  DiskManager& disk_manager,
                  FreeSpaceManager& free_space_manager,
                  LogManager* log_manager = nullptr);

Page* FetchPage(page_id_t page_id);
bool UnpinPage(page_id_t page_id, bool is_dirty,
               std::uint64_t txn_id = 0);
bool FlushPage(page_id_t page_id);
void FlushAllPages();
Page* NewPage(page_id_t* page_id);
bool DeletePage(page_id_t page_id);
bool UndoTransaction(std::uint64_t txn_id);
```

**Lifetime and pinning contract**

- `DiskManager`, `FreeSpaceManager`, and the optional LogManager must outlive the buffer pool.
- Successful `FetchPage()` and `NewPage()` calls return a pinned page.
- The caller must unpin each successful fetch/allocation.
- Page-byte mutations are detected at unpin. When a LogManager is configured, the buffer pool appends a full before/after `PAGE_UPDATE` and tracks the latest page LSN in frame metadata.
- `txn_id == 0` marks a redo-only system update; nonzero IDs associate the update with a user transaction.
- Dirty eviction and explicit flush synchronize WAL through the frame's page LSN before writing the database page.
- Dirty pinned pages are not flushed; a flush attempt on one throws rather than racing with a caller mutating its page bytes.

## 3. Record and table storage

### 3.1 RecordPage

Header: `src/record/record_page.h`

`RecordPage` stores opaque record payloads in one page. It does not encode SQL values or know a table schema.

```cpp
static bool Init(Page* page);
static bool IsValid(const Page* page);

static bool Insert(Page* page, page_id_t page_id,
                   const std::vector<std::uint8_t>& record, RID* rid);
static bool Get(const Page* page, page_id_t page_id, const RID& rid,
                std::vector<std::uint8_t>* record);
static bool Update(Page* page, page_id_t page_id, const RID& rid,
                   const std::vector<std::uint8_t>& record);
static bool Delete(Page* page, page_id_t page_id, const RID& rid);

static bool HasSpace(const Page* page, std::size_t record_size);
static bool Compact(Page* page);

static page_id_t GetNextPageId(const Page* page);
static bool SetNextPageId(Page* page, page_id_t next_page_id);
```

Current implementation characteristics:

- Page size is `PAGE_SIZE` (4096 bytes).
- The page contains a header, a slot directory, and packed record payloads.
- Slot identifiers are paired with generation numbers in `RID`.
- Deleted slots may be reused with a new generation; exhausted generation values are retired rather than wrapped.
- Compaction relocates payload bytes while preserving slot identity.
- An update that cannot fit leaves the existing record unchanged.
- Record payloads must be non-empty and do not span pages.
- `HasSpace()` takes payload size; slot-directory overhead is accounted for internally when a new slot is needed.

The byte layout is an implementation format in `record_page.cpp`; do not treat it as a separately approved persistent-format decision without team review.

### 3.2 TableHeap

Header: `src/record/table_heap.h`

```cpp
static bool Create(BufferPoolManager& bpm, page_id_t* first_page_id,
                   txn_id_t txn_id = 0);

TableHeap(BufferPoolManager& bpm, page_id_t first_page_id,
          TransactionManager* transaction_manager = nullptr);

bool Insert(const std::vector<std::uint8_t>& record, RID* rid,
            txn_id_t txn_id = 0);
bool Get(const RID& rid, std::vector<std::uint8_t>* record,
         txn_id_t txn_id = 0);
bool Scan(std::vector<std::pair<RID, std::vector<std::uint8_t>>>* records,
          txn_id_t txn_id = 0);
bool Update(const RID& rid, const std::vector<std::uint8_t>& record,
            txn_id_t txn_id = 0);
bool Delete(const RID& rid, txn_id_t txn_id = 0);

page_id_t GetFirstPageId() const;
```

- `Create()` allocates and initializes the first record page.
- The constructor opens an existing heap from its first page ID.
- The heap traverses linked record pages and allocates another page when needed.
- The caller must retain the first page ID to reopen the heap.
- The implementation uses BufferPoolManager and RecordPage; callers must obey the buffer-pool pin/unpin contract.
- Mutating operations accept an optional transaction ID and pass it to BufferPoolManager when unpinning modified pages. Pass the active transaction's ID to make those page updates undoable by TransactionManager; the default 0 denotes a redo-only system update.
- When constructed with a `TransactionManager*`, transactional reads acquire shared page locks and writes acquire exclusive page locks. Locks are retained by Strict 2PL until commit/abort. Traversal acquires locks from the first heap page toward the target; insert locks each candidate page exclusively and locks a newly allocated page before modifying it.
- With no TransactionManager attached, TableHeap preserves its original unlocked behavior; callers using nonzero transaction IDs in that mode remain responsible for locking.

### 3.3 RecordTable

Header: `src/record/record_table.h`

`RecordTable::Get()` and `RecordTable::Scan()` accept an optional transaction ID and forward it to `TableHeap`, just as `Insert()`, `Update()`, and `Delete()` forward IDs for writes. The underlying heap must have been constructed with the matching `TransactionManager` for automatic page locking.

## 4. Catalog and schema validation

Sources: `src/catalog/catalog.h`, `src/catalog/validator.h`

### 4.1 Schema types

```cpp
struct ColumnSchema {
    std::string name;
    sql::ColumnType type = sql::ColumnType::INT;
};

struct TableSchema {
    std::string name;
    std::vector<ColumnSchema> columns;

    int ColumnIndex(const std::string& column_name) const;
};
```

`ColumnIndex()` returns a zero-based column index or `-1` if the column is absent. Identifier normalization is ASCII case-insensitive.

### 4.2 Catalog

```cpp
void Catalog::CreateTable(const sql::CreateTableStatement& stmt);
bool Catalog::HasTable(const std::string& name) const;
const TableSchema* Catalog::GetTable(const std::string& name) const;
std::vector<std::string> Catalog::TableNames() const;
std::size_t Catalog::NumTables() const;
```

- The Catalog currently stores table definitions in memory only.
- `CreateTable()` registers a parsed CREATE TABLE definition and throws `CatalogError` for catalog errors such as duplicate tables, no columns, or duplicate column names.
- `GetTable()` returns `nullptr` when the table is absent.
- Table names are returned in alphabetical order, retaining their original spelling.
- Catalog definitions are not persisted to the database file.

### 4.3 Semantic validation

Header: `src/catalog/validator.h`

```cpp
void Validate(const sql::Statement& stmt, const Catalog& catalog);
```

`Validate()` checks a statement against the current Catalog and throws `SemanticError` for the first semantic problem. It does not execute statements, mutate the Catalog, or access storage.

The implemented validation scope includes table/column existence and type/count checks for the statement forms represented in the AST. `CREATE TABLE` registration remains the responsibility of `Catalog::CreateTable()`.

## 5. SQL front end

Sources: `src/sql/tokenizer.h`, `src/sql/ast.h`, `src/sql/parser.h`

### 5.1 Tokenizer

```cpp
std::vector<Token> Tokenize(const std::string& sql);
const char* TokenTypeName(TokenType type);
```

The tokenizer produces tokens and an ending `END_OF_INPUT` token. It throws `TokenizerError` for lexical errors. Keywords are case-insensitive; string literals use single quotes, with doubled single quotes representing one quote.

### 5.2 AST and parser

The AST defines:

- `ColumnType`: `INT`, `TEXT`
- `Value`: integer (`std::int64_t`) or string
- `CompareOp`: `EQ`, `NE`, `LT`, `GT`, `LE`, `GE`
- `LogicalOp`: `AND`, `OR`
- Statement variants: `CreateTableStatement`, `InsertStatement`, `SelectStatement`, `UpdateStatement`, `DeleteStatement`

Parser entry point:

```cpp
sql::Statement sql::Parse(const std::string& sql);
```

- Parses exactly one statement.
- Throws `TokenizerError` for lexical problems and `ParseError` for grammar problems.
- The supported grammar is a limited educational SQL subset: CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, and simple comparison conditions joined by AND/OR.
- The parser builds an AST; it does not validate table contents, execute queries, access pages, or acquire locks.
- WHERE conditions are represented as a flat ordered list of comparisons and connectors. Precedence/parentheses semantics must be settled by the executor contract; do not assume the parser has encoded AND-before-OR precedence.
- There is no SQL execution engine in the current source snapshot.

## 6. B+Tree index

Header: `src/index/bplus_tree.h`

```cpp
using Key = std::int64_t;

explicit BPlusTree(int max_keys = 4);
bool Insert(Key key, const RID& rid);
bool Search(Key key, RID* rid) const;
std::vector<std::pair<Key, RID>> RangeScan(Key low, Key high) const;

std::size_t Size() const;
bool Empty() const;
int Height() const;
bool CheckInvariants(std::string* error = nullptr) const;
```

- This is an in-memory B+Tree using C++ nodes, not database pages.
- Keys are `std::int64_t`; values are `RID`.
- Duplicate keys are rejected by `Insert()`.
- Search and inclusive range scan are implemented.
- `Remove()`, persistence, Buffer Pool integration, and thread safety are not implemented.
- The key policy and current in-memory representation should not be mistaken for a finalized on-disk index format.

## 7. Transactions and concurrency

### 7.1 Shared transaction types

Header: `src/txn/txn_types.h`

- `txn_id_t`: `std::uint64_t`
- `INVALID_TXN_ID`: `0`
- `ResourceId`: alias for `page_id_t`
- `LockMode`: `Shared` or `Exclusive`

Transaction IDs are allocated from 1 in increasing order.

### 7.2 LockManager

Header: `src/txn/lock_manager.h`

```cpp
bool LockShared(txn_id_t txn_id, ResourceId resource);
bool LockExclusive(txn_id_t txn_id, ResourceId resource);
void ReleaseAllLocks(txn_id_t txn_id);
void AbortTransaction(txn_id_t txn_id);
std::vector<std::pair<txn_id_t, txn_id_t>> GetWaitForEdges() const;
bool HoldsLock(txn_id_t txn_id, ResourceId resource, LockMode mode) const;
std::size_t NumLocksHeld(txn_id_t txn_id) const;
```

- Resources are currently page-level because `ResourceId` aliases `page_id_t`.
- Shared locks can coexist; exclusive locks conflict with other transactions' locks.
- Conflicting requests block until granted or until the transaction is marked aborted.
- Waiters are queued FIFO per resource; a lock upgrade receives priority.
- Re-requesting a held lock or requesting a weaker mode succeeds.
- `AbortTransaction()` marks a transaction as aborted for lock requests. The transaction's own worker must subsequently finish cleanup through the TransactionManager.

### 7.3 TransactionManager

Header: `src/txn/transaction_manager.h`

```cpp
using UndoCallback = std::function<bool(txn_id_t)>;

explicit TransactionManager(
    LockManager& lock_manager,
    LogManager* log_manager = nullptr,
    UndoCallback undo_callback = {});

txn_id_t Begin();
bool Commit(txn_id_t txn_id);
bool Abort(txn_id_t txn_id);
bool Lock(txn_id_t txn_id, ResourceId resource, LockMode mode);
bool GetState(txn_id_t txn_id, TransactionState* state) const;
bool HasActiveTransactions() const;
bool RunWhenIdle(const std::function<void()>& action);
```

States are `Active`, `Committed`, and `Aborted`.

- `Commit()` and `Abort()` finish an active transaction and release its locks.
- `Lock()` acquires a lock only for an active transaction.
- `GetState()` returns `false` for an unknown transaction.
- With a `LogManager`, `Begin()` appends BEGIN; `Commit()` appends and synchronizes COMMIT before releasing locks.
- WAL-backed `Abort()` invokes the configured undo callback, appends and synchronizes ABORT, and only then releases locks. If undo or sync fails, it returns false and retains the transaction's locks.
- `BufferPoolManager::UnpinPage(..., txn_id)` associates page updates with the transaction. `TableHeap` and `RecordTable` mutating methods accept an optional transaction ID and pass it through to page unpinning.
- A standalone TransactionManager constructed without a LogManager preserves the earlier in-memory lifecycle behavior.

### 7.4 DeadlockDetector

Header: `src/txn/deadlock_detector.h`

```cpp
explicit DeadlockDetector(LockManager& lock_manager);

static std::vector<txn_id_t> FindCycle(const std::vector<Edge>& edges);
static txn_id_t ChooseVictim(const std::vector<txn_id_t>& cycle);

std::size_t DetectAndResolve();
void Start(std::chrono::milliseconds interval);
void Stop();
std::size_t DeadlocksResolved() const;
```

- Detects cycles in the LockManager wait-for graph.
- Chooses the highest transaction ID in a cycle as victim.
- Marks the victim through `LockManager::AbortTransaction()`.
- The victim's own thread must observe the failed lock request and call `TransactionManager::Abort()`.
- Periodic background detection is supported through `Start()` and `Stop()`.

## 8. WAL, checkpointing, and crash recovery

**Status: implemented on `feature/wal-recovery`; verified by `test_wal_recovery`.**

Files:
- `src/wal/log_manager.h/.cpp`
- `src/storage/recovery_manager.h/.cpp`
- WAL integration in `BufferPoolManager`, `FreeSpaceManager`, `DatabaseHeader`, `TransactionManager`, and `DatabaseInstance`
- `tests/test_wal_recovery.cpp`

### Log format and guarantees

- WAL v2 has a database-identity-bound file header, monotonically increasing LSNs, and checksummed records.
- Record types are `BEGIN`, `PAGE_UPDATE`, `COMMIT`, `ABORT`, and `CHECKPOINT`.
- `PAGE_UPDATE` contains a full 4 KiB before-image followed by a full 4 KiB after-image.
- Transaction ID 0 identifies redo-only system updates (page allocation bitmap, new-page initialization, and database-header changes). Nonzero IDs identify user transaction updates.
- `LogManager::FlushUpTo(lsn)` uses an OS durability primitive (`fsync` on POSIX, `_commit` on Windows).
- Buffer-pool dirty-page writes, bitmap writes, and header updates synchronize the relevant WAL records before writing the database page.
- `TransactionManager::Commit()` synchronizes COMMIT before releasing strict-2PL locks.
- `TransactionManager::Abort()` restores before-images in reverse LSN order, synchronizes ABORT, and only then releases locks.

### Recovery algorithm

- `RecoveryManager` runs after the database identity and matching WAL are validated, but before `FreeSpaceManager`, `DatabaseHeader`, and `BufferPoolManager` load database pages.
- It synchronizes the WAL, redoes page updates in LSN order, undoes a transaction at its ABORT record, and undoes transactions still active at end-of-log.
- After undoing an incomplete transaction, recovery appends and synchronizes an ABORT record. This makes the recovery decision durable and prevents a later restart from undoing that old transaction after newer commits.
- Recovery is idempotent: a crash during recovery can be handled by replaying the same full log again.
- Strict 2PL is required for the ordering rule used by undo: another transaction must not update a page before the previous writer commits or aborts.

### Checkpoint policy and current limits

- `DatabaseInstance::Checkpoint()` requires no active transactions, flushes dirty pages, synchronizes the database file, then appends and synchronizes a CHECKPOINT marker.
- **The current checkpoint does not truncate or recycle the WAL.** Recovery deliberately replays the complete log; this avoids requiring persisted page LSNs or a more advanced checkpoint protocol.
- Page LSNs are held only in buffer-pool frame metadata. They are not persisted in the on-disk page format.
- `TableHeap` and `RecordTable` mutations accept a transaction ID. Inside explicit SQL `BEGIN` / `COMMIT` / `ROLLBACK`, `SqlExecutor` propagates its active transaction ID through the record-table and table-heap paths, so row-page updates participate in WAL undo and strict-2PL locking. Standalone SQL statements outside an explicit transaction use transaction ID 0 (redo-only system updates); automatic one-statement transactions are not implemented.
- Page allocation/deallocation metadata is logged as system updates. Transactional rollback of page allocation/deallocation itself is not implemented; an aborted operation that allocated a page may leave an unreachable allocated page.
- This is a single-process educational recovery implementation, not ARIES and not a production crash-consistency claim. Full-log replay cost grows with WAL size.

### Recovery tests

`test_wal_recovery` covers committed update redo without a data-page flush, WAL-before-data flushing, redo repair of a corrupted committed data page, loser transaction undo after restart, transactional `TableHeap` insert undo, runtime abort followed by a later committed writer to the same page, repeated recovery, checkpoint behavior, and deterministic child-process interruption after a selected redo or undo write, plus a crash after runtime before-image restoration but before ABORT is logged. The runtime-abort case checks reverse-order restoration of multiple updates to one page. Each interrupted-recovery scenario reopens the database twice and verifies the committed state both times. The optional `RecoveryManager` observer runs after a page image is written and supports these deterministic fault-injection tests.

`test_log_manager` covers incomplete final record-header and payload repair, fail-closed handling of malformed record types/lengths/LSNs and complete-record checksum corruption, WAL-header corruption/version rejection, empty/truncated existing WAL rejection, and database-identity mismatch. `test_database_bootstrap` covers interrupted bootstrap/migration and refuses to recreate or repair a missing/truncated WAL for an established database.
`test_wal_crash` also covers process termination after WAL append but before explicit WAL synchronization, after WAL synchronization but before a data-page flush, after a loser page has been flushed, and after durable COMMIT before a data-page flush. The corrupted-page test simulates a torn/corrupt page image; it does not kill the process in the middle of the operating-system write call.

### DatabaseInstance integration

`DatabaseInstance` owns the LogManager, RecoveryManager, FreeSpaceManager, DatabaseHeader, BufferPoolManager, LockManager, and WAL-aware TransactionManager. Startup order is:

1. Open DiskManager and run the database bootstrap/identity check.
2. Open and validate the matching WAL.
3. Run RecoveryManager before loading the bitmap, header, or buffer-pool pages.
4. Construct the remaining database components.

Public integration methods include `GetLogManager()`, `GetTransactionManager()`,
`GetLockManager()`, `GetBufferPoolManager()`, `Flush()`, and `Checkpoint()`.

## 9. Schema-aware RecordSerializer

**Status: implemented and tested.**

Files:
- `src/record/record_serializer.h`
- `src/record/record_serializer.cpp`
- `tests/test_record_serializer.cpp`

`RecordSerializer` converts schema-ordered SQL values into the opaque byte payload stored by `RecordPage` and `TableHeap`, and decodes those payloads back into SQL values. It performs no page I/O.

### Public interface

```cpp
class RecordSerializer {
public:
    static bool Encode(
        const TableSchema& schema,
        const std::vector<sql::Value>& values,
        std::vector<std::uint8_t>* output,
        std::string* error = nullptr);

    static bool Decode(
        const TableSchema& schema,
        const std::vector<std::uint8_t>& input,
        std::vector<sql::Value>* output,
        std::string* error = nullptr);
};
```

### Encoding format

- `INT`: 8-byte signed two's-complement integer, little-endian.
- `TEXT`: 4-byte unsigned length, little-endian, followed by that many raw bytes.
- The schema supplies the field count and types. Schema metadata is not stored in each record.
- Empty text and embedded NUL bytes are supported.
- The current value model does not support NULL.
- Text bytes are preserved as-is; UTF-8 validity is not checked.

### Validation and failure behavior

- Encoding rejects a value count that differs from the schema column count.
- Encoding rejects values whose types do not match their schema columns.
- Empty schemas and unsupported column types are rejected.
- Text values exceeding the 32-bit length limit are rejected.
- Decoding rejects truncated integer fields, truncated text lengths, and truncated text data.
- Decoding rejects trailing bytes after the expected fields.
- Failed encoding and decoding leave the caller's output vector unchanged.
- An optional error string provides failure details.

### Build and test

CMake defines `ocera_record_serializer` and `test_record_serializer`.

The serializer test covers mixed INT/TEXT round trips, integer boundary values, empty and binary-containing text, invalid types and counts, malformed records, output preservation, empty schemas, and null output pointers.

The serialization format is implemented and tested. Treat changes to this format as persisted-record compatibility changes requiring explicit review.

## 10. Build targets in the current CMake configuration

The current `CMakeLists.txt` defines these libraries:

- `ocera_storage`
- `ocera_sql`
- `ocera_txn`
- `ocera_index`
- `ocera_catalog`
- `ocera_record`
- `ocera_table_heap`
- `ocera_record_serializer`
- `ocera_wal`
- `ocera_database_instance`

It defines these executable targets:

- `ocera`
- `test_storage`
- `test_header`
- `test_tokenizer`
- `test_parser`
- `test_buffer_pool`
- `test_lock_manager`
- `test_transaction_manager`
- `test_deadlock_detector`
- `demo_concurrency`
- `test_bplus_tree`
- `test_catalog`
- `test_record_page`
- `test_table_heap`
- `test_record_serializer`
- `test_wal_recovery`

The current CMake file does not register these executables with CTest using `add_test()`. The `ocera_record_serializer` library and `test_record_serializer` executable are defined as build targets.

## 11. Dependency direction and integration boundaries

Intended dependency direction:

```text
SQL text
   |
   v
Tokenizer -> Parser / AST -> Catalog Validator
                              |
                              v
                       SQL Executor (not implemented)
                              |
                +-------------+-------------+
                |             |             |
                v             v             v
            TableHeap      B+Tree       TransactionManager
                |                           |
                v                           v
           RecordPage                    LockManager
                |
                v
          BufferPoolManager
                |
                v
            DiskManager
```

`RecordSerializer` converts schema-aware SQL values to and from the opaque record payload used by `TableHeap`/`RecordPage`; it is not a replacement for either storage component.

Keep lower-level modules independent of SQL and transaction policy. SQL should not call raw `DiskManager` I/O. Page-based modules should use the BufferPoolManager. WAL integration must define flush ordering before crash-safety claims are made.

## 12. API change procedure

For a public API or persisted-format change:

1. Inspect existing headers, implementations, callers, and tests.
2. Agree on cross-module contracts with the affected owners.
3. Update implementation and tests together.
4. Update this document in the same change.
5. Build and run the relevant tests; record the exact results.
6. Distinguish implementation status from team approval and from durability guarantees.

## 13. RecordTable — typed row adapter

**Status:** Implemented as a thin adapter over `RecordSerializer` and `TableHeap`.

`RecordTable` owns a copy of a `TableSchema` and holds a reference to an existing
`TableHeap`. It exposes typed `Insert`, `Get`, `Update`, and `Delete` operations
using schema-ordered `std::vector<sql::Value>` values and generation-aware `RID`s.

- `Insert` encodes values with `RecordSerializer::Encode`, then calls
  `TableHeap::Insert`.
- `Get` retrieves opaque bytes with `TableHeap::Get`, then decodes them using
  `RecordSerializer::Decode`.
- `Update` encodes the replacement values before calling `TableHeap::Update`.
- `Delete` delegates to `TableHeap::Delete`.

The adapter does not own the heap, allocate pages, persist catalog metadata,
execute SQL statements, or add transaction/WAL behavior. The caller must keep
the referenced `TableHeap` alive for the lifetime of the `RecordTable`, and
must supply the same compatible schema when reopening stored records.

CMake targets: `ocera_record_table`, `test_record_table`.

## 14. SQL Executor — initial CREATE TABLE / INSERT support

**Status:** Initial implementation scope; verify with the local build and integration test before marking tested.

Files:
- `src/sql/executor.h`
- `src/sql/executor.cpp`
- `tests/test_sql_executor.cpp`

`SqlExecutor` receives references to an existing in-memory `Catalog` and
`BufferPoolManager`. It parses SQL text or accepts an existing AST statement.
The initial supported statements are `CREATE TABLE` and `INSERT`.

- `CREATE TABLE` validates the statement, creates and initializes a first
  `TableHeap` page, creates a `RecordTable` using the statement schema, and
  registers the schema with the Catalog.
- If Catalog registration fails after page allocation, the executor removes
  its runtime registration and attempts to deallocate the new first page.
- `INSERT` validates table existence, value count, and value types, then
  delegates typed insertion to `RecordTable`.
- The optional inserted `RID` identifies the stored row.
- `GetRowByRID()` is a storage-level row lookup helper for integration and
  internal use; it is not SQL `SELECT`.

The executor owns runtime `TableHeap` and `RecordTable` objects. The caller
must keep the referenced Catalog and BufferPoolManager alive longer than the
executor. The executor is not thread-safe and does not acquire transaction
locks.

The Catalog and executor's table-to-heap registry are in-memory only. Table
pages may be flushed, but this implementation does not persist Catalog
metadata or reopen tables automatically after process restart. WAL,
rollback, transaction integration, `SELECT`, `UPDATE`, and `DELETE` are not
implemented by this milestone.

CMake targets: `ocera_sql_executor`, `test_sql_executor`.


## Change log

| Date | Change |
|---|---|
| 2026-10-14 | Existing baseline documentation separated implemented interfaces from proposals. |
| 2026-11-07 | Consolidated duplicated sections; updated record, table, Catalog, SQL, B+Tree, transaction, and build-target status from `feature/indexing-sql`; added the schema-aware serializer as an explicitly unimplemented proposal. |
| 2026-11-07 | Documented the implemented and tested schema-aware RecordSerializer and its encoding contract. |
| 2026-11-08 | Documented SQL executor foreign-key enforcement and RESTRICT semantics. |


## 11. Foreign-key metadata (feature/indexing-sql)

The current SQL/Catalog implementation extends `CreateTableStatement` with:

```cpp
struct ForeignKeyConstraint {
    std::vector<std::string> columns;
    std::string referenced_table;
    std::vector<std::string> referenced_columns;
    std::size_t position = 0;
};

struct ForeignKeySchema {
    std::vector<std::string> columns;
    std::string referenced_table;
    std::vector<std::string> referenced_columns;
};
```

`CreateTableStatement::foreign_key_constraints` contains parsed declarations.
`TableSchema::foreign_key_constraints` contains validated Catalog metadata.
Identifiers stored in Catalog metadata use the original spelling of the registered
table and column names.

Supported declaration syntax is table-level:

```sql
FOREIGN KEY (child_col [, child_col ...])
REFERENCES parent_table (parent_col [, parent_col ...])
```

Rules enforced during `Catalog::CreateTable()` and `Validate()`:

- Both column lists must be non-empty and have equal lengths.
- Local and referenced column names must be unique within their respective lists.
- All local columns and referenced columns must exist.
- The referenced table must already be registered, except that a table may
  reference its own candidate key.
- The referenced column tuple, in the declared order, must exactly match a
  PRIMARY KEY or UNIQUE constraint on the referenced table.
- Corresponding local and referenced columns must have the same `ColumnType`.
- Invalid definitions are rejected before the new table is published.

The Catalog remains in-memory only. The SQL executor enforces these relationships
for DML operations using row scans:

- `INSERT` rejects a child row unless its referenced tuple exists.
- `UPDATE` validates the complete candidate row set before writing; both outgoing
  references and references from other tables to the updated table are checked.
- `DELETE` validates the complete post-delete row set before deleting; referenced
  parent rows cannot be removed while child rows still point to them.
- Self-referencing and composite foreign keys use the same tuple comparison rules.

The initial referential action is `RESTRICT`: parent-key updates and parent-row
deletes are rejected when they would leave a child reference unresolved. `CASCADE`
and `SET NULL` are not supported. The current SQL value model has no NULL
representation.

Constraint validation happens before DML writes, so a foreign-key rejection does
not partially apply the requested statement. This is statement-level prevalidation,
not transaction rollback or crash atomicity: an underlying storage failure while
writing multiple rows can still leave a partial operation. The executor remains
single-threaded/non-thread-safe and is not integrated with transaction locks or WAL.

## 15. Clean-restart table metadata persistence

**Status:** Implemented for normal shutdown/restart; crash recovery is not provided.

`TableSchema::first_page_id` records the first page of a table's `TableHeap`.
`CatalogStorage` saves table names, column types, primary/unique keys, foreign
keys, index descriptions (metadata only; actual B+Tree structures are not persisted or reconstructed), and each table's first page ID into a linked set of
catalog pages. The first catalog page is referenced by
`DatabaseHeader::GetCatalogRootPageId()`.

Public operations:

- `CatalogStorage::Save(catalog, bpm, header, error)` writes a replacement
  catalog page chain, flushes it, updates the header pointer, then attempts to
  release the previous chain.
- `CatalogStorage::Load(catalog, bpm, header, error)` restores catalog metadata.
- `SqlExecutor(catalog, bpm, &header)` loads metadata and reopens each table heap.

The SQL shell uses this constructor. Existing two-argument `SqlExecutor`
construction remains available for isolated/in-memory tests. A successful
`CREATE TABLE` saves the updated table list. Normal shell exit flushes dirty
table pages. This is clean-shutdown persistence only: no WAL, atomic commit,
torn-write protection, or recovery after an unclean process termination is
provided. The catalog page format has its own format marker; changes to it
require compatibility review and tests.
