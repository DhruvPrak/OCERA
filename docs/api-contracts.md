# OCERA — API Contracts Between Modules

This document describes the public interfaces present on `feature/indexing-sql` and separates implemented functionality from proposals and missing integration work.

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
explicit DiskManager(const std::string& db_file);
~DiskManager();

void ReadPage(page_id_t page_id, char* page_data);
void WritePage(page_id_t page_id, const char* page_data);
page_id_t GetNumPages() const;
```

- Opens or creates the database file.
- Reads and writes one `PAGE_SIZE` page.
- Unwritten pages are read as zero-filled buffers.
- The implementation protects file operations with a mutex.
- The manager is non-copyable.

### 2.2 FreeSpaceManager

Header: `src/storage/free_space_manager.h`

```cpp
explicit FreeSpaceManager(DiskManager& disk_manager);

page_id_t AllocatePage();
void DeallocatePage(page_id_t page_id);
bool IsAllocated(page_id_t page_id) const;
```

- Page 0 stores the allocation bitmap; page 1 is reserved for the database header.
- A single page-sized bitmap represents at most `PAGE_SIZE * 8` page IDs.
- `AllocatePage()` throws `std::runtime_error` when the bitmap is full.
- Callers must not deallocate reserved pages.

### 2.3 DatabaseHeader

Header: `src/storage/database_header.h`

Current format constants:

```cpp
static constexpr char kMagic[4] = {'M', 'D', 'B', '1'};
static constexpr std::uint32_t kCurrentVersion = 1;
```

Public interface:

```cpp
explicit DatabaseHeader(DiskManager& disk_manager);

std::uint32_t GetVersion() const;
std::uint32_t GetPageSize() const;
page_id_t GetCatalogRootPageId() const;
```

Header fields are serialized at fixed offsets rather than by writing a raw C++ structure. The header stores file metadata; it does not implement crash recovery.

### 2.4 BufferPoolManager

Header: `src/storage/buffer_pool_manager.h`

```cpp
BufferPoolManager(std::size_t pool_size,
                  DiskManager& disk_manager,
                  FreeSpaceManager& free_space_manager);

Page* FetchPage(page_id_t page_id);
bool UnpinPage(page_id_t page_id, bool is_dirty);
bool FlushPage(page_id_t page_id);
void FlushAllPages();
Page* NewPage(page_id_t* page_id);
bool DeletePage(page_id_t page_id);
```

**Lifetime and pinning contract**

- `DiskManager` and `FreeSpaceManager` must outlive the buffer pool.
- Successful `FetchPage()` and `NewPage()` calls return a pinned page.
- The caller must call `UnpinPage()` exactly once for every successful fetch or allocation.
- A `Page*` may be accessed only while its page remains pinned. Do not use the pointer after unpinning.
- `UnpinPage(id, true)` marks the page dirty. Unpinning with `false` does not clear an existing dirty flag.
- Unpinning an unknown page or a page with a zero pin count returns `false`.

**Replacement, flushing, and concurrency**

- LRU replacement is implemented; pinned frames cannot be evicted.
- Pinned pages cannot be deleted.
- Dirty pages are written during eviction, `FlushPage()`, or `FlushAllPages()`.
- A dirty flag is cleared only after the corresponding write succeeds.
- Flush operations may write pinned pages. Callers must prevent concurrent changes to page bytes while a page is being flushed.
- The buffer-pool mutex protects internal metadata, not direct access to `Page::data`.
- Higher layers must coordinate concurrent page-content access.
- The buffer pool does not automatically flush all pages during destruction; explicitly flush when required.
- WAL flush ordering is not implemented. Do not infer crash consistency from buffer-pool flushing.

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
static bool Create(BufferPoolManager& bpm, page_id_t* first_page_id);

TableHeap(BufferPoolManager& bpm, page_id_t first_page_id);

bool Insert(const std::vector<std::uint8_t>& record, RID* rid);
bool Get(const RID& rid, std::vector<std::uint8_t>* record);
bool Update(const RID& rid, const std::vector<std::uint8_t>& record);
bool Delete(const RID& rid);

page_id_t GetFirstPageId() const;
```

- `Create()` allocates and initializes the first record page.
- The constructor opens an existing heap from its first page ID.
- The heap traverses linked record pages and allocates another page when needed.
- The caller must retain the first page ID to reopen the heap.
- The implementation uses BufferPoolManager and RecordPage; callers must obey the buffer-pool pin/unpin contract.
- The current implementation is not transaction-integrated and does not provide page-level latching or concurrent table operations.
- A successful explicit buffer-pool flush followed by reopening was exercised by the TableHeap test. This is not a claim of crash-safe persistence or WAL recovery.

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
explicit TransactionManager(LockManager& lock_manager);

txn_id_t Begin();
bool Commit(txn_id_t txn_id);
bool Abort(txn_id_t txn_id);
bool Lock(txn_id_t txn_id, ResourceId resource, LockMode mode);
bool GetState(txn_id_t txn_id, TransactionState* state) const;
```

States are `Active`, `Committed`, and `Aborted`.

- `Commit()` and `Abort()` finish an active transaction and release its locks.
- `Lock()` acquires a lock only for an active transaction.
- `GetState()` returns `false` for an unknown transaction.
- `Abort()` currently changes transaction state and releases locks; it does not undo modified database pages.
- Commit does not provide WAL-backed durability in the current implementation.

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

**Status: not implemented in the reviewed branch snapshot.**

The following are requirements for future design, not current guarantees:

- Log records and LSN assignment.
- Write-ahead ordering between durable log records and dirty-page writes.
- Commit durability.
- Undo/redo recovery.
- Checkpoint format and restart behavior.
- Page LSNs and the required flush protocol.

Transaction abort currently releases locks and changes transaction state; it does not roll back page changes. Buffer-pool flushing alone must not be described as crash recovery.

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

## Change log

| Date | Change |
|---|---|
| 2026-10-14 | Existing baseline documentation separated implemented interfaces from proposals. |
| 2026-11-07 | Consolidated duplicated sections; updated record, table, Catalog, SQL, B+Tree, transaction, and build-target status from `feature/indexing-sql`; added the schema-aware serializer as an explicitly unimplemented proposal. |
| 2026-11-07 | Documented the implemented and tested schema-aware RecordSerializer and its encoding contract. |
