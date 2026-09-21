# OCERA — Team Roles & Responsibilities

## Team Members

| Role | Name | GitHub Handle | Primary Module(s) |
|---|---|---|---|
| Team Lead | _Member 1 (placeholder)_ | _placeholder_ | Storage Engine + Buffer Pool Manager |
| Member 2 | _Member 2 (placeholder)_ | _placeholder_ | Transaction Manager + Lock Manager + Deadlock Detection |
| Member 3 | _Member 3 (placeholder)_ | _placeholder_ | SQL Parser + B+Tree Index + WAL/Recovery |

> Replace placeholder names and GitHub handles once confirmed.

## Module Ownership

### Team Lead — Storage Engine + Buffer Pool
- Page format and raw disk I/O (read/write fixed-size pages)
- Free space / page allocation manager
- Buffer pool with LRU eviction
- Overall integration and architecture decisions across modules

### Member 2 — Transaction Manager + Lock Manager
- Transaction lifecycle (begin / commit / abort)
- Two-Phase Locking (2PL) implementation
- Deadlock detection (wait-for graph, cycle detection, victim selection)

### Member 3 — SQL Parser + Indexing + Recovery
- Mini SQL parser (SELECT, INSERT, UPDATE, DELETE)
- B+Tree index implementation
- Write-ahead log (WAL) and checkpointing
- Crash recovery logic

### Shared Responsibilities (everyone)
- Writing tests for their own module
- Contributing to benchmarking (LRU vs FIFO, throughput under load)
- Final report + architecture diagrams + demo prep
- Keeping `docs/architecture-decisions.md` and `docs/api-contracts.md` up to date when their
  module's interface changes

## Review Rules
- [ ] Every pull request needs **at least 1 review** from another teammate before merging to `main`
- [ ] If a PR changes a function signature listed in `api-contracts.md`, update that file **in the
      same PR**
- [ ] No direct pushes to `main` — always branch + PR
- [ ] Branch naming: `feature/<module>-<short-description>` (e.g. `feature/buffer-pool-lru`)

> _Placeholder: adjust review rules to match how strict your team wants to be — e.g. 2 reviewers
> for cross-module changes._

## Communication Rules
- **Decisions:** any design decision that affects more than one module goes into
  `docs/architecture-decisions.md` — not just discussed verbally/in chat and forgotten
- **Blockers:** if you're stuck on something another module owns, tag that teammate directly
  rather than guessing at their interface
- **Sync frequency:** _placeholder — e.g. "short check-in twice a week"_
- **Where we talk:** _placeholder — e.g. WhatsApp group / Discord server_
- **Meeting notes:** _placeholder — link to shared notes doc if you keep one_

## Milestones & Ownership Checkpoints
Refer to the 4-month roadmap in `project-brief.md`. Each month, every member should confirm their
module milestone is met before moving to the next month's work — flag delays early rather than at
the end.
