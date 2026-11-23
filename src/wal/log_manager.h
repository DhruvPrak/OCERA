#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../common/page.h"
#include "../common/database_id.h"

namespace ocera {

enum class LogRecordType : uint8_t {
    BEGIN = 1,
    PAGE_UPDATE = 2,
    COMMIT = 3,
    ABORT = 4,
    CHECKPOINT = 5
};

struct LogRecord {
    LogRecordType type = LogRecordType::BEGIN;
    uint64_t lsn = 0;
    uint64_t txn_id = 0;
    page_id_t page_id = INVALID_PAGE_ID;
    std::vector<uint8_t> payload;
};

// Append-only write-ahead log. Its v2 file header binds it to one DatabaseId.
// PAGE_UPDATE payloads contain exactly two PAGE_SIZE images: before-image
// followed by after-image.
class LogManager {
public:
    LogManager(const std::string& wal_file, const DatabaseId& database_id,
               bool allow_create_if_missing = false,
               bool allow_repair_incomplete_header = false);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    bool AppendRecord(LogRecordType type,
                      uint64_t txn_id,
                      page_id_t page_id,
                      const std::vector<uint8_t>& payload,
                      uint64_t* lsn_out = nullptr);

    bool AppendBegin(uint64_t txn_id, uint64_t* lsn_out = nullptr);
    bool AppendPageUpdate(uint64_t txn_id, page_id_t page_id,
                          const char* before_image, const char* after_image,
                          uint64_t* lsn_out = nullptr);
    bool AppendCommit(uint64_t txn_id, uint64_t* lsn_out = nullptr);
    bool AppendAbort(uint64_t txn_id, uint64_t* lsn_out = nullptr);
    bool AppendCheckpoint(uint64_t* lsn_out = nullptr);

    // Reads all complete, valid records. An incomplete final record is
    // discarded; corruption in a complete record is reported as failure.
    bool Scan(std::vector<LogRecord>* records);

    // Makes all records through lsn durable. This uses an OS sync primitive,
    // not just the C/C++ stream flush operation.
    bool FlushUpTo(uint64_t lsn);

    uint64_t GetLastLSN() const;
    std::string GetLastError() const;
    const DatabaseId& GetDatabaseId() const { return database_id_; }

private:
    bool AppendRecordLocked(LogRecordType type, uint64_t txn_id,
                            page_id_t page_id,
                            const std::vector<uint8_t>& payload,
                            uint64_t* lsn_out);
    bool ScanLocked(std::vector<LogRecord>* records, bool repair_tail);
    bool SyncFileLocked();
    bool SetErrorLocked(const std::string& message);

    std::string wal_file_;
    DatabaseId database_id_{};
    std::FILE* file_ = nullptr;
    uint64_t last_lsn_ = 0;
    uint64_t durable_lsn_ = 0;
    bool healthy_ = true;
    std::string last_error_;
    mutable std::mutex mutex_;
};

}  // namespace ocera
