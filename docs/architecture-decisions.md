# OCERA — Architecture Decisions Log

This file tracks every meaningful technical decision the team makes, so nobody has to remember
"wait, why did we do it this way?" three weeks later — and so Claude (or anyone else) can look
this up instead of guessing.

**Rule:** whenever the team agrees on something non-trivial (a format, an algorithm choice, a
module boundary), add a row here **before** writing the code for it.

## How to use this table
- **Date** — when the decision was made (YYYY-MM-DD)
- **Decision** — one clear sentence
- **Reason** — why, in a few words
- **Affected Modules** — which parts of OCERA this touches
- **Status** — `Proposed` / `Confirmed` / `Superseded` / `Reverted`

---

## Decisions

| Date | Decision | Reason | Affected Modules | Status |
|---|---|---|---|---|
| _2026-01-01 (EXAMPLE)_ | Use fixed 4KB page size | Matches typical OS page size, simplifies disk I/O math | Storage Engine, Buffer Pool | Example — replace me |
| _2026-01-05 (EXAMPLE)_ | Use LRU (not FIFO or Clock) for page replacement | Best balance of simplicity and performance for a student project | Buffer Pool | Example — replace me |
| _2026-01-10 (EXAMPLE)_ | Use Strict 2PL for concurrency control | Guarantees serializability, easier to reason about than optimistic CC | Lock Manager, Transaction Manager | Example — replace me |
| _placeholder_ | _placeholder_ | _placeholder_ | _placeholder_ | Proposed |

> ⚠️ The three rows above are **examples only** to show the format — delete or replace them with
> your team's real decisions once you start making them.

## Open Questions (not yet decided)
- [ ] C++ or Java — final call?
- [ ] Exact page size (4KB assumed — confirm)
- [ ] Will joins be supported, or single-table queries only?
- [ ] WAL format: simple append-only log vs. more structured log records?

> Add to this list any time a design question comes up in a team discussion but isn't resolved yet.
