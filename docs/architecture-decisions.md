# OCERA — Architecture Decisions Log

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
| P-05 | Use wait-for graph cycle detection for deadlocks | Directly demonstrates OS/DBMS deadlock concepts | Lock Manager, Transaction Manager | Proposed |
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
- [ ] Team timing: Ishika's recovery ownership remains the current role assignment, but exact Month 4 integration handoff should be confirmed by all three.

## Rules for recording decisions

1. Discuss cross-module choices with all affected owners before merging code that depends on them.
2. Change a proposal to Confirmed only after explicit team agreement.
3. Include the reason and affected modules.
4. If a decision changes, mark the old row Superseded and add the replacement; do not silently erase history.
5. Keep implementation evidence separate from planned features.
