#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "../src/wal/log_manager.h"

using namespace ocera;
#define CHECK(x) do { if (!(x)) { std::cerr << "CHECK failed line " << __LINE__ << ": " #x << '\n'; return 1; } } while (false)

int main() {
    const std::string path = "test_log_manager.wal";
    std::remove(path.c_str());
    const DatabaseId database_id = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint64_t begin_lsn = 0, update_lsn = 0, commit_lsn = 0;
    char before[PAGE_SIZE]{};
    char after[PAGE_SIZE]{};
    std::memcpy(before, "before", 7);
    std::memcpy(after, "after", 6);

    {
        LogManager log(path, database_id, true);
        CHECK(log.AppendBegin(41, &begin_lsn));
        CHECK(log.AppendPageUpdate(41, 7, before, after, &update_lsn));
        CHECK(log.AppendCommit(41, &commit_lsn));
        CHECK(begin_lsn == 1 && update_lsn == 2 && commit_lsn == 3);
        CHECK(log.FlushUpTo(commit_lsn));
        std::vector<LogRecord> records;
        CHECK(log.Scan(&records));
        CHECK(records.size() == 3);
        CHECK(records[1].type == LogRecordType::PAGE_UPDATE);
        CHECK(records[1].page_id == 7);
        CHECK(records[1].payload.size() == 2U * PAGE_SIZE);
        CHECK(std::memcmp(records[1].payload.data(), before, PAGE_SIZE) == 0);
        CHECK(std::memcmp(records[1].payload.data() + PAGE_SIZE, after, PAGE_SIZE) == 0);
    }
    {
        LogManager reopened(path, database_id);
        CHECK(reopened.GetLastLSN() == 3);
        uint64_t next = 0;
        CHECK(reopened.AppendBegin(42, &next));
        CHECK(next == 4);
        CHECK(reopened.FlushUpTo(next));
    }
    // A torn final record is discarded on reopen, without losing earlier records.
    {
        std::ofstream tail(path, std::ios::binary | std::ios::app);
        const char partial[] = {1, 0, 0};
        tail.write(partial, sizeof(partial));
    }
    {
        LogManager repaired(path, database_id);
        CHECK(repaired.GetLastLSN() == 4);
        std::vector<LogRecord> records;
        CHECK(repaired.Scan(&records));
        CHECK(records.size() == 4);
    }

    // A damaged complete record must fail closed rather than be silently accepted.
    const std::string corrupt_path = "test_log_manager_corrupt.wal";
    std::remove(corrupt_path.c_str());

    const std::string mismatch_path = "test_log_manager_mismatch.wal";
    std::remove(mismatch_path.c_str());
    {
        LogManager original(mismatch_path, database_id, true);
        CHECK(original.AppendBegin(77));
        CHECK(original.FlushUpTo(original.GetLastLSN()));
    }
    DatabaseId other_id = database_id;
    other_id[0] ^= 0xFFU;
    bool mismatch_rejected = false;
    try { LogManager wrong_database(mismatch_path, other_id); }
    catch (const std::runtime_error&) { mismatch_rejected = true; }
    CHECK(mismatch_rejected);
    std::remove(mismatch_path.c_str());
    {
        LogManager clean(corrupt_path, database_id, true);
        CHECK(clean.AppendBegin(9));
        CHECK(clean.FlushUpTo(clean.GetLastLSN()));
    }
    {
        std::fstream corrupt(corrupt_path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(32 + 32);
        const char bad = 0x5A;
        corrupt.write(&bad, 1);
    }
    bool rejected = false;
    try { LogManager invalid(corrupt_path, database_id); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);

    std::remove(path.c_str());
    std::remove(corrupt_path.c_str());

    std::cout << "LogManager tests passed.\n";
    return 0;
}
