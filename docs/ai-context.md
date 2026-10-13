# OCERA — AI Context for Claude Projects

**Last reconciled:** 2026-10-10
**Repository/source of truth:** https://github.com/DhruvPrak/OCERA

## What this project is

OCERA means **Optimized Concurrent & Crash-Resilient Database Engine**. It is a four-month, three-person Semester V CSE educational database engine in C++17. It demonstrates OS and DBMS topics through implementation: fixed-size pages, disk I/O, free-space management, buffering, indexing, SQL, transactions, locks, deadlocks, WAL and crash recovery.

This is a constrained student project, not a production database. Explain concepts in simple language first, then technical detail. Avoid redesigns and stretch goals unless the team explicitly asks.

## Current status

Month 1 storage foundation is in `main`: 4096-byte pages, DiskManager, bitmap-based FreeSpaceManager, DatabaseHeader, and storage/header tests. The previously reported clean build and tests passed, but rerun tests in the current checkout before claiming a fresh verification.

Not implemented yet: Buffer Pool/LRU, B+Tree, records/tables/Catalog, SQL parser/executor, Transaction Manager/Lock Manager, 2PL, deadlock detection, WAL, checkpointing, recovery, full integration and benchmarks.

## Team ownership and schedule

- **Dhruv Prakash:** storage engine (Month 1 complete), Buffer Pool/LRU (Month 2), integration coordination.
- **Ishika Singh:** B+Tree/indexing, records/tables/Catalog and SQL front-end (Month 2); primary WAL/recovery owner under current assignment (Month 4).
- **Bhavya Goel:** Transaction Manager, Lock Manager, 2PL, deadlock detection and concurrency tests (Month 3).

Ownership is not the same as month number. Dhruv owns the Buffer Pool even though it is Month 2 work. Ishika's exact Month 4 handoff with Bhavya/Dhruv must be coordinated because recovery depends on transaction and storage contracts.

## Which documents to trust

- `docs/project-brief.md`: status, scope and four-month roadmap.
- `docs/team-roles.md`: owner and delivery window for each module.
- `docs/api-contracts.md`: verified storage APIs and proposed higher-layer contracts.
- `docs/architecture-decisions.md`: confirmed facts vs proposals and open questions.
- `CONTRIBUTING.md`: Git workflow.

Existing source and tests establish what is actually implemented. The docs contain proposals for future work; a proposed signature or design is not proof of implementation or team approval.

## How to assist

1. Inspect the latest `main` and relevant files before giving code instructions.
2. State clearly whether something is **implemented**, **proposed**, **in progress**, or **not yet implemented**.
3. Use exact existing names from source for implemented APIs. For future APIs, follow `docs/api-contracts.md` only after checking whether the team approved that proposal.
4. If an API or architecture decision is open, ask the owner/team or offer a clearly labeled proposal; do not silently invent it.
5. Prefer small, testable C++17 changes. Preserve the existing storage format and working tests unless a change is explicitly agreed.
6. Give exact build/test commands and expected outcomes. Do not claim tests passed unless they were run.
7. Do not implement a teammate's module without coordination. Cross-module changes require updating the contract and informing affected owners.
8. No direct commits to `main`; use a feature branch and pull request.

## Roadmap

- **Month 1 — complete:** page-based storage, DiskManager, free-space management, database header and tests.
- **Month 2:** Buffer Pool/LRU (Dhruv), B+Tree, records/tables, Catalog and SQL front-end foundation (Ishika).
- **Month 3:** transactions, locks/2PL, deadlock detection/recovery and concurrency tests (Bhavya).
- **Month 4:** WAL, checkpointing, crash recovery, integration, benchmarks, documentation and demo (Ishika primary for recovery; all owners support integration).

## Parallel work rule

Before coding against another module, check its API in `docs/api-contracts.md`. Use mocks for independent work if necessary, but do not assume mocks define the final interface until the team approves it. Keep shared documents in sync with merged code.
