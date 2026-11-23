#include "storage/database_instance.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>

using namespace ocera;

namespace {

void ExecuteOrFail(SqlExecutor& executor, const std::string& sql) {
    std::string error;
    if (!executor.Execute(sql, nullptr, &error)) {
        throw std::runtime_error(sql + " failed: " + error);
    }
}

void ExpectFailure(SqlExecutor& executor, const std::string& sql) {
    std::string error;
    if (executor.Execute(sql, nullptr, &error)) {
        throw std::runtime_error(sql + " unexpectedly succeeded");
    }
    if (error.empty()) {
        throw std::runtime_error(sql + " failed without an error message");
    }
}

std::size_t CountRows(SqlExecutor& executor, const std::string& sql) {
    QueryResult result;
    std::string error;
    if (!executor.Execute(sql, nullptr, &error, &result)) {
        throw std::runtime_error(sql + " failed: " + error);
    }
    return result.rows.size();
}

std::string FirstText(SqlExecutor& executor, const std::string& sql) {
    QueryResult result;
    std::string error;
    if (!executor.Execute(sql, nullptr, &error, &result)) {
        throw std::runtime_error(sql + " failed: " + error);
    }
    if (result.rows.size() != 1 || result.rows[0].empty()) {
        throw std::runtime_error("query did not return exactly one value: " + sql);
    }
    return result.rows[0][0].string_value;
}

}  // namespace

int main() {
    const auto unique_id =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto db_path = std::filesystem::temp_directory_path() /
        ("ocera_sql_transactions_" + std::to_string(unique_id) + ".db");
    const std::string wal_path = db_path.string() + ".wal";

    try {
        {
            DatabaseInstance database(
                db_path.string(), 16, DatabaseOpenMode::CREATE_NEW);
            SqlExecutor& first = database.GetExecutor();

            ExecuteOrFail(first,
                "CREATE TABLE students (id INT PRIMARY KEY, name TEXT);");
            ExecuteOrFail(first,
                "CREATE INDEX students_id_idx ON students(id);");

            std::cout << "[1] BEGIN / own-write visibility / ROLLBACK\n";
            ExecuteOrFail(first, "BEGIN TRANSACTION;");
            ExecuteOrFail(first,
                "INSERT INTO students VALUES (1, 'uncommitted');");
            assert(CountRows(first,
                "SELECT id FROM students WHERE id = 1;") == 1);
            ExecuteOrFail(first, "ROLLBACK;");
            assert(CountRows(first,
                "SELECT id FROM students;") == 0);

            std::cout << "[2] COMMIT persists INSERT and UPDATE\n";
            ExecuteOrFail(first, "BEGIN;");
            ExecuteOrFail(first,
                "INSERT INTO students VALUES (2, 'before');");
            ExecuteOrFail(first, "COMMIT TRANSACTION;");
            assert(CountRows(first,
                "SELECT id FROM students WHERE id = 2;") == 1);

            ExecuteOrFail(first, "BEGIN;");
            ExecuteOrFail(first,
                "UPDATE students SET name = 'after' WHERE id = 2;");
            ExecuteOrFail(first, "ROLLBACK TRANSACTION;");
            assert(FirstText(first,
                "SELECT name FROM students WHERE id = 2;") == "before");

            std::cout << "[3] DELETE rollback restores the row\n";
            ExecuteOrFail(first, "BEGIN;");
            ExecuteOrFail(first,
                "DELETE FROM students WHERE id = 2;");
            assert(CountRows(first,
                "SELECT id FROM students;") == 0);
            ExecuteOrFail(first, "ROLLBACK;");
            assert(CountRows(first,
                "SELECT id FROM students;") == 1);

            std::cout << "[4] Nested / unmatched transaction controls rejected\n";
            ExpectFailure(first, "COMMIT;");
            ExecuteOrFail(first, "BEGIN;");
            ExpectFailure(first, "BEGIN;");
            ExpectFailure(first,
                "CREATE TABLE forbidden_in_transaction (id INT);");
            ExecuteOrFail(first, "ROLLBACK;");

            std::cout << "[5] Failed DML aborts the explicit transaction\n";
            ExecuteOrFail(first, "BEGIN;");
            ExpectFailure(first,
                "INSERT INTO students VALUES (2, 'duplicate');");
            ExpectFailure(first, "COMMIT;");
            assert(CountRows(first,
                "SELECT id FROM students;") == 1);

            std::cout << "[6] Strict 2PL blocks a conflicting SQL reader until commit\n";
            SqlExecutor second(
                database.GetCatalog(), database.GetBufferPoolManager(),
                &database.GetDatabaseHeader(), &database.GetTransactionManager());
            ExecuteOrFail(first, "BEGIN;");
            ExecuteOrFail(first,
                "INSERT INTO students VALUES (3, 'visible-after-commit');");
            ExecuteOrFail(second, "BEGIN;");

            std::atomic<bool> reader_finished{false};
            std::size_t reader_rows = 0;
            std::string reader_error;
            std::thread reader([&]() {
                QueryResult result;
                const bool ok = second.Execute(
                    "SELECT id FROM students WHERE id = 3;",
                    nullptr, &reader_error, &result);
                if (ok) reader_rows = result.rows.size();
                reader_finished.store(true);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(75));
            const bool was_blocked = !reader_finished.load();
            ExecuteOrFail(first, "COMMIT;");
            reader.join();
            assert(was_blocked);
            assert(reader_finished.load());
            assert(reader_error.empty());
            assert(reader_rows == 1);
            ExecuteOrFail(second, "COMMIT;");

            database.Flush();
        }

        std::cout << "[7] Reopen and verify committed rows + rebuilt index\n";
        {
            DatabaseInstance reopened(
                db_path.string(), 16, DatabaseOpenMode::OPEN_EXISTING);
            QueryResult result;
            std::string error;
            if (!reopened.GetExecutor().Execute(
                    "SELECT name FROM students WHERE id = 2;",
                    nullptr, &error, &result)) {
                throw std::runtime_error("reopened SELECT failed: " + error);
            }
            assert(result.rows.size() == 1);
            assert(result.rows[0][0].string_value == "before");
            assert(CountRows(reopened.GetExecutor(),
                "SELECT id FROM students WHERE id = 3;") == 1);
            reopened.Flush();
        }

        std::filesystem::remove(db_path);
        std::filesystem::remove(wal_path);
        std::cout << "All SQL transaction integration tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SQL transaction integration test failed: "
                  << exception.what() << '\n';
        std::filesystem::remove(db_path);
        std::filesystem::remove(wal_path);
        return 1;
    }
}
