# Contributing to OCERA

These rules let three people work in parallel without accidentally breaking one another's code.

## Branch layout

- `main` — shared integration branch; should build and pass existing tests.
- Use a focused branch from the latest `main`, for example:
  - `feature/buffer-pool` — Dhruv
  - `feature/index-sql` — Ishika
  - `feature/concurrency` — Bhavya
  - `feature/wal-recovery` — Ishika, when that phase begins
  - `docs/reconcile-status-interfaces` — shared documentation work

Existing branches named `feature/storage`, `feature/sql` and `feature/txn` have previously been observed behind `main`. Compare them with the latest `main` before reusing them. Never assume an old branch is current.

## Before starting work

```bash
git checkout main
git pull origin main
git checkout -b feature/<module-name>
```

If the branch already exists, switch to it and bring it up to date with `main` after checking for local changes. Do not delete or overwrite teammates' work.

Read:
- `docs/project-brief.md`
- `docs/team-roles.md`
- `docs/api-contracts.md`
- `docs/architecture-decisions.md`

Confirm your module's contract and tests before coding.

## Daily workflow

```bash
git status
git add <specific-files>
git commit -m "Describe the change"
git push -u origin feature/<module-name>
```

Open a pull request into `main` when a coherent, tested change is ready. Request at least one teammate review. Include:
- What changed and why
- Build and test commands run
- Test outcomes
- Any API or architecture decision changes
- Follow-up work or known limitations

## Cross-module changes

If a public interface changes, update `docs/api-contracts.md` in the same PR and notify the affected owner. If an architecture decision changes, update `docs/architecture-decisions.md`. Avoid parallel edits to the same shared header without first coordinating.

## Build and test

In the Linux container from the repository root:

```bash
mkdir -p build && cd build
cmake -G "Unix Makefiles" ..
cmake --build .
./ocera
./test_storage
./test_header
```

Use the Windows native instructions in `ONBOARDING.md` if not building in Docker. Do not claim tests passed unless you ran them and saw the result.

## Commit and merge rules

- No direct pushes to `main`.
- Keep commits focused and descriptive.
- At least one other teammate reviews each PR.
- Merge only after required tests pass and contract/docs changes are included.
- After a merge, update local `main` and rebase/merge it into your working branch as appropriate; resolve conflicts carefully rather than force-pushing over shared work.
