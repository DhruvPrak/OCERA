# OCERA — Architecture Decisions Log

This document records OCERA's architectural decisions, the reasoning behind them, and their approval status.

**Important:** An implementation detail is not automatically a formally approved team decision. Proposed decisions must remain marked as proposed until the team explicitly confirms them.

## 1. Decision Status

- **Proposed:** Suggested or implemented in code, but explicit team confirmation has not been recorded.
- **Confirmed:** Explicitly agreed upon by the team.
- **Superseded:** Replaced by a later confirmed decision.
- **Reverted:** Intentionally rolled back.

## 2. Decisions and Proposals

| ID | Date | Decision / Proposal | Reason | Affected Modules | Status |
|---|---|---|---|---|---|
| P-01 | 2026-10-14 | Use fixed-size database pages of 4,096 bytes. | Fixed-size pages simplify page addressing and storage operations. | Common types, storage, future buffer pool | Proposed — implemented in code; confirmation pending |
| P-02 | 2026-10-14 | Reserve page 0 for the free-space bitmap and page 1 for the database header. | Reserved pages provide predictable locations for storage metadata. | Storage | Proposed — implemented in code; confirmation pending |
| P-03 | 2026-10-14 | Use page-level locking through `ResourceId = page_id_t`. | Provides a straightforward locking model for the current storage design. | Lock manager, transaction manager, future buffer pool and query executor | Proposed — implemented in code; confirmation pending |
| P-04 | 2026-10-14 | Retain locks until transaction commit or abort, consistent with Strict Two-Phase Locking. | Helps prevent conflicting transactions from accessing locked resources before a transaction finishes. | Lock manager, transaction manager | Proposed — implemented in code; confirmation pending |
| P-05 | 2026-10-14 | Select the youngest transaction, identified by the highest transaction ID, as the deadlock victim. | Provides a deterministic victim-selection rule. | Deadlock detector, transaction manager, lock manager | Proposed — implemented in code; confirmation pending |
| P-06 | 2026-10-14 | Let the deadlock detector mark a victim while the victim's own worker performs transaction-manager cleanup. | Separates deadlock detection from transaction lifecycle cleanup. | Deadlock detector, transaction manager, lock manager | Proposed — implemented in code; confirmation pending |
| P-07 | 2026-10-14 | Do not claim transaction rollback until an undo/recovery mechanism is implemented. | The current abort operation releases locks and records transaction state but does not restore modified page data. | Transaction manager, future WAL and recovery | Proposed — reflects current implementation limitation |

## 3. Open Questions

The following items require review and explicit team agreement.

- [ ] Confirm or revise P-01 through P-07.
- [ ] Finalize the Buffer Pool Manager API, including pinning, unpinning, dirty-page tracking, and eviction.
- [ ] Define how the Buffer Pool Manager interacts with `DiskManager` and `FreeSpaceManager`.
- [ ] Finalize B+Tree page layouts and interfaces with the storage and buffer-pool modules.
- [ ] Define the supported SQL grammar and query execution scope.
- [ ] Define WAL records, log-flush guarantees, checkpoint behavior, and undo/redo responsibilities before claiming durable commits or rollback.
- [ ] Decide whether joins are required project functionality or a stretch goal, considering the four-month deadline.
- [ ] Confirm how transaction workers detect deadlock victim status and perform abort cleanup.
- [ ] Confirm the expected database-file compatibility policy for future format versions.

## 4. Decision-Making Rules

1. Record significant architectural choices in this document.
2. Mark a decision **Confirmed** only after explicit team agreement.
3. Include the rationale and affected modules for each decision.
4. When a confirmed decision changes, mark the previous entry **Superseded** and record the replacement.
5. Update API contracts and tests when an approved decision changes module behavior.
6. Avoid redesigning completed modules unless a verified requirement or integration issue justifies the change.
7. Respect team responsibilities, agreed scope, and the project deadline.
8. Do not claim durability, rollback, or crash recovery guarantees until the corresponding behavior is implemented and tested.

## Change Log

| Date | Change | Status |
|---|---|---|
| 2026-10-14 | Replaced template examples with proposals grounded in the reviewed implementation and added open questions | Review draft; team approval pending |
**Last reconciled:** 2026-10-10

This log distinguishes decisions evidenced by the current implementation from choices that still need explicit team approval. A proposal must not be described as confirmed just because it appears in a plan or code sketch.

## Confirmed by current source

| ID | Decision/fact | Reason or evidence | Affected modules | Status |
|---|---|---|---|---|
| C-01 | Use C++17 and CMake | `CMakeLists.txt` sets required C++ standard to 17 | All | Confirmed |
| C-02 | Database pages are fixed at 4096 bytes | `src/common/config.h` defines `PAGE_SIZE = 4096` | Storage, future Buffer Pool and page-based modules | Confirmed |
| C-03 | Page ID 0 is reserved for the free-space bitmap and page ID 1 for the database header | Shared constants in `src/common/config.h`; storage implementation uses them | Storage, Catalog integration | Confirmed |
| C-04 | `Page` is a zero-initialized raw byte array of `PAGE_SIZE`; invalid page ID is -1 | `src/common/page.h` | All page-based modules | Confirmed |
| C-05 | Database header uses magic `MDB1`, format version 1, explicit field offsets | `src/storage/database_header.h/.cpp` | Storage, file compatibility, recovery | Confirmed |
| C-06 | Free-space tracking currently uses one bitmap page, supporting up to 32,768 page IDs | `src/storage/free_space_manager.h` | Storage | Confirmed current limitation |
| C-07 | DiskManager is the raw file-I/O boundary and exposes fixed-page read/write | `src/storage/disk_manager.h` | All higher layers | Confirmed |

## Proposals to approve before dependent coding

| ID | Proposed decision | Reason | Affected modules | Status |
|---|---|---|---|---|
| P-01 | Use LRU for Buffer Pool replacement | Simple, understandable baseline for the project | Buffer Pool | Proposed |
| P-02 | Keep pin count, dirty flag and replacement metadata outside persisted page bytes | Prevent runtime metadata from corrupting on-disk format | Buffer Pool | Proposed; strong implementation guidance |
| P-03 | Use page-level locking for initial concurrency implementation | Aligns with page-based storage and keeps lock-resource model simple | Transactions, table/index operations | Proposed |
| P-04 | Use Strict Two-Phase Locking | Locks are retained until transaction completion | Lock Manager, Transaction Manager | Implemented in `feature/concurrency`; tests passed; formal team approval status to be confirmed |
| P-05 | Use wait-for graph cycle detection for deadlocks | Directly demonstrates OS/DBMS deadlock concepts | Lock Manager, Transaction Manager | Proposed; wait-for graph and victim-marking hooks exist in `feature/concurrency`, but cycle-detection implementation is not verified |
| P-06 | Limit SQL to basic single-table CRUD and simple predicates; joins are stretch | Protect four-month scope | SQL, executor, tests | Proposed |
| P-07 | Define a minimal record/table-page format and a Catalog before B+Tree/SQL integration | These formats are not implemented yet | Tables, Catalog, B+Tree, SQL | Proposed |
| P-08 | Use an append-only WAL with explicit LSNs and write-ahead flush ordering | Small initial durability design | WAL, Buffer Pool, Transaction Manager | Proposed |
| P-09 | Keep public cross-module APIs in `docs/api-contracts.md` and update them in the same PR as signature changes | Reduce parallel integration mismatch | All | Proposed process; follow unless team objects |

## Open questions — team must decide

- [ ] Buffer Pool: constructor dependencies, pool size, fetch failure semantics, pin/unpin misuse behavior, dirty-page flush rules.
- [ ] Table pages: record types, schema/type representation, slot layout and how rows spanning pages are handled (prefer not to support spanning rows initially).
- [ ] B+Tree: key type, duplicate keys, node layout, root updates and index persistence.
- [ ] SQL: exact grammar subset, supported operators, result representation and error handling.
- [ ] Concurrency: lock granularity, whether conflicting lock requests block, transaction states, strict vs. basic 2PL, deadlock victim policy.
- [ ] Durability: WAL record format, before/after image strategy, redo/undo scope, commit flush policy, page LSNs, checkpoint algorithm and recovery startup sequence.
- [ ] Header format: byte order and compatibility policy for future versions; avoid changing version 1 layout without a documented versioning plan.
- [ ] Team timing: WAL/recovery ownership and the Month 4 integration handoff must be confirmed by all three teammates.

## Rules for recording decisions

1. Discuss cross-module choices with all affected owners before merging code that depends on them.
2. Change a proposal to Confirmed only after explicit team agreement.
3. Include the reason and affected modules.
4. If a decision changes, mark the old row Superseded and add the replacement; do not silently erase history.
5. Keep implementation evidence separate from planned features.
