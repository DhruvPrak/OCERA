# Contributing to OCERA

Simple rules so three people can work on the same repo without stepping on
each other's code.

## Branch layout

- `main` - always buildable. Nobody commits directly here.
- `feature/storage` - Team Lead's work (pages, disk I/O, buffer pool/LRU)
- `feature/txn` - Member 2's work (transaction manager, 2PL, deadlock detection)
- `feature/sql` - Member 3's work (SQL parser, B+Tree, WAL/recovery)

Cross-cutting or shared changes (e.g. something in `src/common/`) can go
through a short-lived branch like `feature/common-<thing>`.

## Daily workflow

```bash
# 1. Make sure your local main is up to date before starting work
git checkout main
git pull

# 2. Switch to your feature branch (create it once, reuse after)
git checkout feature/storage        # or feature/txn / feature/sql

# 3. Bring in anything new that landed on main since you last synced
git merge main
# (resolve any conflicts here if they come up)

# 4. Do your work, then commit as usual
git add .
git commit -m "Implement page read/write to disk"
git push

# 5. When a chunk of work is ready to share with the team,
#    open a Pull Request on GitHub: feature/storage -> main
#    Ask one teammate to glance at it, then merge.
```

## Commit messages

Keep them short and describe *what changed*, not "fixed stuff":
- Good: `Add LRU eviction to buffer pool manager`
- Good: `Fix off-by-one in page offset calculation`
- Avoid: `update`, `wip`, `changes`

## Pull Requests

- Open a PR from your feature branch into `main` whenever a milestone from
  the roadmap is done (not necessarily every single commit).
- One other teammate should at least skim it before merging - catching a
  bug in review is much cheaper than catching it during integration in
  Month 3/4.
- After merging, everyone else should `git checkout main && git pull` then
  `git merge main` into their own feature branch, to stay in sync.

## Setting up your branch for the first time

Whoever is doing this (each teammate, once):
```bash
git clone <REPO_URL>
cd OCERA
git checkout -b feature/<yourmodule>   # e.g. feature/txn
git push -u origin feature/<yourmodule>
```
