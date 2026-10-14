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