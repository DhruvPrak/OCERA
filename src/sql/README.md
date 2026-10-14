# SQL, Indexing and Recovery Module

**Owner:** Ishika Singh
**Planned delivery:** SQL/indexing foundation in Month 2; WAL/recovery in Month 4
**Status:** Not implemented yet; this file documents planned scope only.

## Responsibilities
- Minimum record/table layout and Catalog needed by the SQL subset
- B+Tree index
- Mini SQL parser and execution path for basic single-table CRUD
- Write-Ahead Logging, checkpointing and crash recovery in Month 4, coordinated with Bhavya's transaction module and Dhruv's Buffer Pool/storage

## Dependencies
Use `docs/api-contracts.md` and `docs/architecture-decisions.md` as the shared contract/decision log. SQL execution must not bypass the Buffer Pool to call raw DiskManager I/O directly. WAL design must agree with commit durability, dirty-page flushing and recovery semantics.

## Before coding
Agree the supported SQL grammar, record/page format, B+Tree key policy, Catalog metadata and WAL/transaction hooks with affected teammates. These details are not yet finalized.

See `docs/project-brief.md` for the roadmap and `CONTRIBUTING.md` for the branch/PR workflow.
