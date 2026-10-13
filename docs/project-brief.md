# OCERA — Project Brief

**Last reconciled:** 2026-10-10
**Repository:** https://github.com/DhruvPrak/OCERA
**Primary branch:** `main`
**Project:** four-month, three-person Semester V CSE project combining Operating Systems and DBMS.

## What is OCERA?

OCERA means **Optimized Concurrent & Crash-Resilient Database Engine**. It is a small educational embedded database engine built in C++ to make core database and OS ideas visible: fixed-size pages, buffering, indexing, transactions, locking, deadlock handling, logging and recovery.

The goal is a credible, testable learning engine—not a production replacement for SQLite. Keep the implementation achievable within the four-month schedule.

## Current status

### Completed — Month 1 storage foundation

The storage foundation has been merged into `main`. The current code includes:

- `src/common/config.h`: `PAGE_SIZE = 4096`, bitmap page ID 0 and header page ID 1.
- `src/common/page.h`: `page_id_t`, `INVALID_PAGE_ID`, and a zero-initialized fixed-size `Page`.
- `src/storage/disk_manager.h/.cpp`: fixed-size page reads/writes and page count.
- `src/storage/free_space_manager.h/.cpp`: bitmap-based page allocation, deallocation and allocation checks.
- `src/storage/database_header.h/.cpp`: file magic/version/page-size/catalog-root metadata in reserved page 1.
- `tests/test_storage.cpp` and `tests/test_header.cpp`, included in CMake.

Reported tests passed: page allocation returned IDs 2, 3, and 4; page write/read succeeded; a freed page was reused; unwritten page reads returned zeros; header tests passed. Re-run the build/tests locally before treating a fresh checkout as verified.

The bitmap uses one 4 KiB page and therefore tracks up to 32,768 page IDs (128 MiB at 4 KiB per page) in the current design. Larger-file bitmap chaining is future work, not part of the immediate scope.

### Not implemented yet

- Buffer Pool Manager and LRU replacement
- B+Tree
- Record/page layout, table access and Catalog
- SQL parser and query execution
- Transaction lifecycle and Lock Manager
- Two-Phase Locking (2PL), deadlock detection and concurrency tests
- Write-Ahead Logging (WAL), checkpointing and crash recovery
- End-to-end integration, benchmarks and final presentation

The `src/txn/README.md` and `src/sql/README.md` files are module placeholders, not implementations.

## Team ownership versus delivery schedule

Ownership means the teammate is accountable for a module. The roadmap month is the target window for building and integrating it; the roadmap does not reassign ownership.

| Owner | Owned work | Planned delivery |
|---|---|---|
| Dhruv Prakash | Storage engine; Buffer Pool Manager/LRU; integration coordination | Storage in Month 1 (complete); Buffer Pool in Month 2 |
| Ishika Singh | B+Tree/indexing; records/tables, Catalog and SQL front-end; primary owner for WAL/recovery unless revised by the team | Indexing/SQL foundation in Month 2; WAL/recovery in Month 4 |
| Bhavya Goel | Transaction Manager, Lock Manager, 2PL, deadlock detection and concurrency tests | Month 3; can develop against mock interfaces while other modules are unfinished |

Month 4 recovery is dependent on storage and transaction interfaces. Ishika owns recovery implementation under the current role assignment; Dhruv supports storage integration and Bhavya supports transaction/recovery semantics. Confirm this division at the next team sync.

## Four-month roadmap

| Month | Milestones | Exit evidence |
|---|---|---|
| 1 — Storage foundation | Page-based file layout, DiskManager, free-space management, allocation, database header | Build succeeds; storage/header tests pass — reported complete |
| 2 — Buffering, indexing and SQL foundation | Buffer Pool/LRU (Dhruv); B+Tree, records/tables, Catalog, parser/front-end (Ishika) | Unit tests for pin/unpin, eviction, index operations, catalog and supported SQL subset |
| 3 — Concurrency control | Transaction begin/commit/abort, lock manager, 2PL, synchronization, deadlock detection/recovery | Tests for conflicting transactions, serializable schedules, lock release and deadlock cases |
| 4 — Durability and wrap-up | WAL, checkpointing, crash recovery, integration, benchmarks, docs and demo | Crash/restart tests, end-to-end tests, recorded benchmark results and final report |

Parallel work is allowed, but integration must follow agreed APIs. Do not make multiple modules depend on invented signatures.

## Scope

### Minimum viable project

- Single-process/single-machine embedded database with file-based storage
- Fixed 4 KiB pages
- A buffer pool with a simple replacement policy
- One index type: B+Tree
- Basic single-table CRUD SQL: SELECT, INSERT, UPDATE and DELETE
- Transaction lifecycle and lock-based concurrency control
- WAL/checkpoint/recovery sufficient for a demonstrated crash-recovery scenario
- Automated unit and integration tests

### Stretch goals only if the core works

- Joins, broader SQL syntax, query optimization beyond simple rules, additional benchmark comparisons

### Out of scope

Distributed/multi-node operation, a client-server networking layer, full SQL-standard compliance, authentication/permissions, and GUI. Do not add a load balancer or a complex query optimizer unless the mentor explicitly changes the scope.

## Tech stack and workflow

- C++17; CMake; standard library threads/mutexes
- Binary database file and fixed-size pages
- GitHub repository and feature branches with pull requests
- Docker/Dev Containers recommended for a consistent environment; native build is also supported

See `ONBOARDING.md` for setup and `CONTRIBUTING.md` for Git workflow.

## Source of truth

1. Existing code and tests determine what is implemented.
2. This brief records current status and schedule.
3. `docs/team-roles.md` records ownership.
4. `docs/api-contracts.md` records current/proposed cross-module interfaces.
5. `docs/architecture-decisions.md` records confirmed decisions and open proposals.

A proposal in the API or decisions document is not a confirmed team decision until the team approves it.
