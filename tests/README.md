# tests

Test scripts and crash-simulation scripts will live here (Month 3-4).


## WAL and crash recovery

Build and run the recovery integration test:

```sh
cmake --build build --target test_wal_recovery
./build/test_wal_recovery
```

The test covers durable commit redo, WAL-before-data flushing, repair of a
corrupted committed data page, loser undo, transactional TableHeap updates,
runtime abort ordering, repeated recovery, checkpoint markers, and restoration
of a damaged database header from WAL. It also launches child processes that
terminate immediately after a selected redo or undo write, plus after runtime
undo has restored multiple updates to one page but before ABORT is logged. It
then verifies that reopening (twice) completes recovery correctly.

The WAL parser/corruption matrix is covered by `test_log_manager`, including
repair of incomplete final record headers/payloads and rejection of malformed
or checksum-invalid complete records, invalid WAL headers, and mismatched
DatabaseIds. Bootstrap and migration failure cases are covered by
`test_database_bootstrap`.

`test_wal_crash` separately exercises process termination after WAL append but
before WAL sync, after WAL sync but before page flush, after a loser page flush,
and after a durable commit before page flush.

## SQL transactions

Build and run the SQL transaction integration test:

```sh
cmake --build build --target test_sql_transactions
./build/test_sql_transactions
```

The test covers SQL `BEGIN`, `COMMIT`, and `ROLLBACK`, visibility of a
transaction's own writes, rollback of INSERT/UPDATE/DELETE, failed-DML abort,
DDL rejection inside transactions, page-lock blocking between SQL readers and
writers, and clean-restart persistence of committed rows.
