# Transaction and Concurrency Module

**Owner:** Bhavya Goel
**Planned delivery:** Month 3
**Status:** Implemented on `feature/concurrency`; not yet integrated into `main`. Transaction and lock manager tests have passed on the feature branch. Deadlock cycle detection is not yet verified.

## Responsibilities
- Transaction lifecycle: begin, commit and abort
- Lock Manager and the agreed 2PL policy
- Deadlock detection and victim handling
- Unit tests for lock compatibility, transaction lifecycle, conflicts and deadlocks

## Dependencies
Use the shared contracts in `docs/api-contracts.md`. Coordinate lock resource granularity and commit/abort hooks with Ishika (SQL/WAL) and Dhruv (Buffer Pool/storage). Abort durability depends on the WAL/recovery design; do not assume undo logging already exists.

## Before coding
Confirm the proposed APIs and open choices in `docs/architecture-decisions.md` with the team. Implement independent logic against mocks if needed, then integrate against approved interfaces.

See `docs/project-brief.md` for the roadmap and `CONTRIBUTING.md` for the branch/PR workflow.
