<p align="center">
  <img src="assets/logo/ocera-logo.png" alt="OCERA" width="500">
</p>

<h1 align="center">OCERA</h1>

<p align="center">
  <strong>Optimized Concurrent & Crash-Resilient Database Engine</strong>
</p>

# OCERA

**Optimized Concurrent & Crash-Resilient Database Engine**
A concurrent, crash-proof embedded database engine, built from scratch to
demonstrate core Operating Systems (paging, scheduling, synchronization,
deadlock handling) and DBMS (transactions, locking, indexing, recovery, ACID)
concepts.

> "A tiny database that behaves like a real one - safe, fast, and
> crash-proof, built entirely by us."

## Status

🟡 Environment setup (Month 1, Week 1)

## Team

| Role | Member | Owns |
|---|---|---|
| Team Lead | _Dhruv Prakash_ | `src/storage/` - page format, disk I/O, buffer pool (LRU) |
| Member 2 | _Bhavya Goel_ | `src/txn/` - transaction manager, 2PL lock manager, deadlock detection |
| Member 3 | _Ishika Singh_ | `src/sql/` - SQL parser, B+Tree index, WAL, crash recovery |

## Building (Docker - recommended, same for every teammate)

Requirements: Docker Desktop installed and running.

**Option 1 - VS Code (easiest):** open this folder in VS Code, install the
"Dev Containers" extension if prompted, then press `Ctrl+Shift+P` ->
"Dev Containers: Reopen in Container". VS Code rebuilds and opens a
terminal already inside the container, with this folder mounted at
`/workspace`.

**Option 2 - plain Docker commands:**
```bash
docker build -t ocera-dev .
docker run -it --rm -v "$(pwd):/workspace" ocera-dev
# you're now inside the container, in /workspace
mkdir -p build && cd build
cmake -G "Unix Makefiles" ..
cmake --build .
./ocera
```

If everything is set up correctly, `ocera` runs a threading sanity
check and prints:

```
Environment OK - threads + mutex working correctly.
```

## Building (native, without Docker)

Requirements: a C++17 compiler, CMake 3.15+, and a threads library
(installed via MSYS2 on Windows - see team setup notes for the exact steps).

```bash
mkdir build
cd build
cmake -G "MinGW Makefiles" ..   # on Linux/Mac use: cmake ..
cmake --build .
./ocera.exe   # on Linux/Mac: ./ocera
```

## Roadmap

| Month | Milestones |
|---|---|
| 1 | Page format + file layout design; raw disk I/O; free-space/page allocation manager |
| 2 | Buffer pool manager (LRU eviction); B+Tree index; mini SQL parser (single-table CRUD) |
| 3 | Transaction manager (begin/commit/abort); 2PL locking; deadlock detector |
| 4 | Write-ahead logging + checkpointing + crash recovery; benchmarking; final report + demo |

## Project layout

```
src/
  storage/   - pages, disk I/O, buffer pool (Team Lead)
  txn/       - transactions, locking, deadlock detection (Member 2)
  sql/       - SQL parser, B+Tree, WAL/recovery (Member 3)
  common/    - shared types/constants used across modules
  main.cpp   - entry point (currently a threading sanity check)
tests/       - test scripts, crash-simulation scripts
docs/        - architecture diagrams, design notes
```
