# OCERA — API Contracts Between Modules

This file defines how OCERA's internal modules talk to each other. If you're building the
Transaction Manager and need to call into the Lock Manager, this is the doc that tells you (and
Claude) exactly what function to call and what it returns — without needing to read someone
else's source code.

**Rule:** update this file whenever a function signature changes. Treat it like a contract —
if you break it, tell the team.

---

## Storage Engine
Handles raw reads/writes of fixed-size pages to disk.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `readPage(pageId)` | `pageId: int` | `Page` (raw bytes) | Reads one fixed-size page from disk |
| `writePage(pageId, data)` | `pageId: int`, `data: byte[]` | `success: bool` | Writes/overwrites a page |
| `allocatePage()` | — | `pageId: int` | Allocates a new page, updates free-space map |
| `freePage(pageId)` | `pageId: int` | `success: bool` | Marks a page as free |

_Placeholder: confirm page size and on-disk file layout before finalizing signatures._

---

## Buffer Pool
Keeps hot pages in memory, evicts least-used pages (LRU) when full.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `getPage(pageId)` | `pageId: int` | `Page*` (pinned) | Returns page from cache or loads from disk |
| `unpinPage(pageId, isDirty)` | `pageId: int`, `isDirty: bool` | `success: bool` | Releases a page after use |
| `flushPage(pageId)` | `pageId: int` | `success: bool` | Forces a dirty page to disk |
| `evictPage()` | — | `pageId: int` (evicted) | Internal — picks LRU victim when pool is full |

_Placeholder: decide pool size (number of frames) and pin/unpin discipline._

---

## Page Manager
Sits between Storage Engine and higher layers — manages page format and free space.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `getFreePage()` | — | `pageId: int` | Finds/allocates a free page |
| `markUsed(pageId)` | `pageId: int` | `success: bool` | Updates free-space bitmap |
| `getPageHeader(pageId)` | `pageId: int` | `PageHeader` | Slot count, free space offset, etc. |

_Placeholder: define exact page header format (fields + byte offsets)._

---

## SQL Parser
Turns SQL text into a query plan / parse tree.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `parse(sqlString)` | `sql: string` | `ParseTree` or `ParseError` | Supports SELECT/INSERT/UPDATE/DELETE only |
| `validate(parseTree, schema)` | `ParseTree`, `Schema` | `bool`, `errors[]` | Checks table/column existence |

_Placeholder: decide grammar subset — WHERE clause complexity, supported operators._

---

## Query Executor
Runs a validated query plan against the storage layer.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `execute(plan, txnId)` | `QueryPlan`, `txnId: int` | `ResultSet` | Runs SELECT/INSERT/UPDATE/DELETE |
| `scan(tableId)` | `tableId: int` | `RowIterator` | Full table scan |
| `indexLookup(indexId, key)` | `indexId: int`, `key` | `RowIterator` | B+Tree lookup path |

_Placeholder: confirm whether joins are in scope (see project-brief.md scope section)._

---

## Transaction Manager
Manages transaction lifecycle: begin, commit, abort.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `beginTransaction()` | — | `txnId: int` | Starts a new transaction |
| `commitTransaction(txnId)` | `txnId: int` | `success: bool` | Commits, triggers WAL flush |
| `abortTransaction(txnId)` | `txnId: int` | `success: bool` | Rolls back changes via log |

_Placeholder: define transaction states (Active, Partially Committed, Committed, Failed, Aborted)._

---

## Lock Manager
Implements 2PL; grants/releases locks; feeds the deadlock detector.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `acquireLock(txnId, resourceId, mode)` | `txnId`, `resourceId`, `mode: SHARED/EXCLUSIVE` | `granted: bool` (or blocks) | Blocks if conflicting lock held |
| `releaseLock(txnId, resourceId)` | `txnId`, `resourceId` | `success: bool` | Called at commit/abort (Strict 2PL) |
| `getWaitForGraph()` | — | `Graph` | Used by deadlock detector |

_Placeholder: decide granularity — row-level, page-level, or table-level locking._

---

## WAL / Recovery
Write-ahead log + crash recovery (redo/undo) + checkpointing.

| Function (placeholder) | Input | Output | Notes |
|---|---|---|---|
| `logWrite(txnId, pageId, before, after)` | `txnId`, `pageId`, before/after images | `lsn: int` | Appends a log record, returns log sequence number |
| `checkpoint()` | — | `success: bool` | Flushes state, truncates old log |
| `recover()` | — | `success: bool` | Runs on startup — redo committed, undo uncommitted |

_Placeholder: decide log record format (physical vs logical logging)._

---

## Change Log for This File
| Date | Change | By |
|---|---|---|
| _placeholder_ | Initial draft created | _placeholder_ |
