# OCERA — Team Roles & Responsibilities

**Last reconciled:** 2026-10-10

## Team

| Member | Role | Primary ownership | Delivery window |
|---|---|---|---|
| Dhruv Prakash | Team lead | Storage engine, Buffer Pool Manager/LRU, cross-module integration | Storage Month 1 complete; Buffer Pool Month 2 |
| Ishika Singh | Module owner | B+Tree/indexing, record/table and Catalog layer, SQL front-end; primary WAL/recovery owner under current assignment | Indexing/SQL Month 2; WAL/recovery Month 4 |
| Bhavya Goel | Module owner | Transaction Manager, Lock Manager, 2PL, deadlock detection and concurrency tests | Month 3 |

Ownership and schedule are separate concepts. The monthly roadmap states when a module is targeted; the ownership table states who is accountable for it. This reconciles the earlier docs: Dhruv owns the Buffer Pool even though it is Month 2 work; Bhavya owns transaction control in Month 3; Ishika owns SQL/indexing in Month 2 and is the current primary owner for WAL/recovery in Month 4.

## Work boundaries

### Dhruv — Storage and Buffer Pool

- Maintain the existing page format, DiskManager, free-space manager and database header.
- Implement Buffer Pool Manager and replacement policy after agreeing on the API in `docs/api-contracts.md`.
- Own integration coordination and resolve cross-module API conflicts with the team.
- Avoid changing the existing on-disk layout without a documented decision and migration/test plan.

### Ishika — Indexing, Tables/Catalog, SQL and Recovery

- Implement B+Tree/indexing and the minimum record/table layout and Catalog needed for basic single-table CRUD.
- Implement a deliberately small SQL subset and its execution path; don't promise full SQL compatibility.
- Current assignment: own WAL, checkpointing and crash recovery, pending explicit confirmation by all three teammates. Coordinate log-record needs with Bhavya and page-flush needs with Dhruv.
- Agree on page formats, transaction hooks and recovery interfaces before implementing code that depends on them.

### Bhavya — Transactions and Concurrency

- Implement transaction lifecycle, lock manager, 2PL policy, deadlock detection and focused concurrency tests.
- Start against the published interface and small mocks if storage, SQL or WAL dependencies are not ready.
- Coordinate commit/abort hooks with Ishika's WAL/recovery design. Do not invent undo/rollback durability semantics before the team agrees on the WAL contract.
- Keep transaction code independent of SQL parsing; the SQL executor should call the transaction API.

## Shared responsibilities

- Each owner writes unit tests for their module and documents how to run them.
- All three contribute to integration tests, benchmarks, diagrams, final report and demo.
- Any cross-module signature change requires a same-PR update to `docs/api-contracts.md`.
- Any architectural choice affecting multiple modules must be added to `docs/architecture-decisions.md`.
- All teammates and their AI assistants should use the same updated documents.

## Git and review rules

- `main` is the shared integration branch. Do not commit directly to it.
- Work in focused feature branches created from the latest `main`.
- Open a pull request to `main`; at least one other teammate reviews it.
- Keep PRs small enough to test and review. Include build/test results.
- Suggested branches: `feature/buffer-pool`, `feature/index-sql`, `feature/concurrency`, and `feature/wal-recovery`.
- Existing `feature/storage`, `feature/sql` and `feature/txn` branches were previously found behind `main`; inspect and update them before reuse. Do not assume they contain current code.

## Team checkpoints

1. Approve the proposed interface decisions in `docs/api-contracts.md` and `docs/architecture-decisions.md`.
2. Each owner confirms scope, files, tests and dependencies before coding.
3. Review integration-ready PRs against the shared contract.
4. At each milestone, record what passed, what remains, and any change to the roadmap.
