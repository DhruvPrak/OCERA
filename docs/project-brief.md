# OCERA — Project Brief

## What is OCERA?
OCERA is a small, educational database engine built from scratch by a 3-person student team.
Think of it as a tiny SQLite — it stores data on disk in pages, supports basic SQL, runs multiple
transactions safely at once, and can recover from a crash without losing data.

This project is built for a semester V CSE curriculum, combining **Operating Systems** concepts
(paging, scheduling, synchronization, deadlocks) with **DBMS** concepts (transactions, locking,
indexing, recovery, ACID).

## Main Goals
- Never lose data, even if the system crashes mid-operation
- Handle multiple users/transactions at once without corrupting data
- Run efficiently on limited memory without slowing down as data grows
- Make every core DB concept (paging, locking, recovery, indexing) visible and understandable,
  not hidden inside a massive codebase

## Expected Features
- [ ] Fixed-size page storage on disk (raw binary files)
- [ ] Buffer pool manager with LRU page eviction
- [ ] B+Tree index for fast row lookups
- [ ] Mini SQL parser supporting SELECT, INSERT, UPDATE, DELETE
- [ ] Query execution engine (relational algebra: select, project, join)
- [ ] Transaction manager (begin / commit / abort)
- [ ] Lock manager with Two-Phase Locking (2PL)
- [ ] Deadlock detection via wait-for graph + victim selection
- [ ] Write-Ahead Logging (WAL) + checkpointing
- [ ] Crash recovery (redo/undo on restart)
- [ ] Benchmarking: LRU vs FIFO, throughput under concurrent load

> Update the checkboxes as features get built.

## Tech Stack
- **Language:** C++ (Java acceptable alternative)
- **Concurrency:** pthreads / `std::thread` with mutexes & semaphores
- **Storage:** Raw binary files on disk
- **Build tools:** CMake / Makefile
- **Version control:** Git + GitHub
- **Testing:** Custom test scripts + crash simulation (`kill -9` mid-transaction)

## Project Scope

### In Scope
- Single-node, single-machine embedded database (not client-server, not distributed)
- Core CRUD SQL on single tables (joins may be a stretch goal — mark status below)
- One index type (B+Tree)
- File-based storage, no networking layer

### Out of Scope (for now)
- [ ] Distributed / multi-node support
- [ ] Full SQL standard compliance
- [ ] Query optimizer beyond basic rule-based choices
- [ ] Authentication / user permissions
- [ ] GUI (command-line only)

> _Placeholder: revisit this list once Month 2 milestones are done — some "out of scope" items
> may become stretch goals if time allows._

## Current Status
- **Phase:** _placeholder — e.g. "Month 1: raw disk I/O + page allocation"_
- **Last updated:** _placeholder date_
