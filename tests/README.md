# tests

Test scripts and crash-simulation scripts will live here (Month 3-4).


## WAL and crash recovery

Build and run the recovery integration test:

```sh
cmake --build build --target test_wal_recovery
./build/test_wal_recovery
```

The test covers durable commit redo, WAL-before-data flushing, loser undo,
transactional TableHeap updates, runtime abort ordering, repeated recovery,
checkpoint markers, and restoration of a damaged database header from WAL.

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
