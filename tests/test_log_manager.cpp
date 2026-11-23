#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/wal/log_manager.h"

using namespace ocera;
#define CHECK(x) do { if (!(x)) { std::cerr << "CHECK failed line " << __LINE__ << ": " #x << '\n'; return 1; } } while (false)

namespace {
std::vector<char> ReadFile(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& file, const std::vector<char>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("could not write WAL corruption fixture");
}

void PutU64LE(std::vector<char>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        bytes.at(offset + i) = static_cast<char>(value >> (8U * i));
}

bool RejectsWal(const std::string& file, const DatabaseId& id) {
    try {
        LogManager invalid(file, id);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int TestWalCorruptionMatrix(const DatabaseId& id) {
    const std::string fixture = "test_log_manager_matrix.wal";
    std::remove(fixture.c_str());
    {
        LogManager log(fixture, id, true);
        char before[PAGE_SIZE]{};
        char after[PAGE_SIZE]{};
        std::memcpy(before, "before", 7);
        std::memcpy(after, "after", 6);
        if (!log.AppendBegin(101) ||
            !log.AppendPageUpdate(101, 2, before, after) ||
            !log.AppendCommit(101) ||
            !log.FlushUpTo(log.GetLastLSN())) return 1;
    }
    const std::vector<char> valid = ReadFile(fixture);
    constexpr std::size_t file_header = 32;
    constexpr std::size_t record_header = 36;
    const std::size_t update_offset = file_header + record_header;

    // An incomplete final record header is safely discarded, preserving BEGIN.
    auto truncated_header = valid;
    truncated_header.resize(update_offset + 12);
    WriteFile(fixture, truncated_header);
    {
        LogManager repaired(fixture, id);
        if (repaired.GetLastLSN() != 1) return 2;
    }
    if (std::filesystem::file_size(fixture) != update_offset) return 3;

    // A complete header with an incomplete final payload is also a repairable tail.
    auto truncated_payload = valid;
    truncated_payload.resize(update_offset + record_header + 128);
    WriteFile(fixture, truncated_payload);
    {
        LogManager repaired(fixture, id);
        if (repaired.GetLastLSN() != 1) return 4;
    }
    if (std::filesystem::file_size(fixture) != update_offset) return 5;

    // Complete-record corruption must fail closed, including corruption in a
    // middle record rather than only at end-of-file.
    auto invalid_type = valid;
    invalid_type[update_offset] = static_cast<char>(0x7f);
    WriteFile(fixture, invalid_type);
    if (!RejectsWal(fixture, id)) return 6;

    auto invalid_length = valid;
    invalid_length[update_offset + 4] = 0;
    invalid_length[update_offset + 5] = 0;
    invalid_length[update_offset + 6] = 0;
    invalid_length[update_offset + 7] = 0;
    WriteFile(fixture, invalid_length);
    if (!RejectsWal(fixture, id)) return 7;

    auto invalid_lsn = valid;
    PutU64LE(invalid_lsn, update_offset + 8, 99);
    WriteFile(fixture, invalid_lsn);
    if (!RejectsWal(fixture, id)) return 8;

    auto invalid_checksum = valid;
    invalid_checksum[update_offset + record_header + 17] ^= 0x40;
    WriteFile(fixture, invalid_checksum);
    if (!RejectsWal(fixture, id)) return 9;

    // Existing databases must not silently initialize empty or truncated headers.
    WriteFile(fixture, {});
    if (!RejectsWal(fixture, id)) return 10;
    auto truncated_file_header = valid;
    truncated_file_header.resize(11);
    WriteFile(fixture, truncated_file_header);
    if (!RejectsWal(fixture, id)) return 11;

    auto invalid_file_header_crc = valid;
    invalid_file_header_crc[28] ^= 0x01;
    WriteFile(fixture, invalid_file_header_crc);
    if (!RejectsWal(fixture, id)) return 12;

    auto unsupported_file_version = valid;
    unsupported_file_version[8] = 0x7f;
    WriteFile(fixture, unsupported_file_version);
    if (!RejectsWal(fixture, id)) return 13;

    std::remove(fixture.c_str());
    return 0;
}
}  // namespace

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

    const int corruption_result = TestWalCorruptionMatrix(database_id);
    CHECK(corruption_result == 0);

    std::cout << "LogManager tests passed (including WAL corruption matrix).\n";
    return 0;
}
