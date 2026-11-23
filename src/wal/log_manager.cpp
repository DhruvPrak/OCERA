#include "log_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ocera {
namespace {
constexpr size_t kFileHeaderSize = 32;
constexpr size_t kRecordHeaderSize = 36;
constexpr uint32_t kFormatVersion = 2;
constexpr uint32_t kMaxPayloadSize = 2U * PAGE_SIZE;
constexpr char kMagic[8] = {'O','C','E','R','A','W','A','L'};

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8U * i)));
}
void PutU64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (8U * i)));
}
uint32_t GetU32(const uint8_t* p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8U * i);
    return v;
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8U * i);
    return v;
}
uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
bool ValidType(uint8_t type) { return type >= 1 && type <= 5; }

bool TruncateFile(std::FILE* file, uint64_t size) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_SET) != 0) return false;
    return _chsize_s(_fileno(file), size) == 0;
#else
    return ftruncate(fileno(file), static_cast<off_t>(size)) == 0;
#endif
}
bool Seek(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
bool FileSize(std::FILE* file, uint64_t* size) {
#ifdef _WIN32
    const auto end = _ftelli64(file);
    if (end < 0 || _fseeki64(file, 0, SEEK_END) != 0) return false;
    const auto result = _ftelli64(file);
#else
    const auto end = ftello(file);
    if (end < 0 || fseeko(file, 0, SEEK_END) != 0) return false;
    const auto result = ftello(file);
#endif
    (void)end;
    if (result < 0) return false;
    *size = static_cast<uint64_t>(result);
    return true;
}
}  // namespace

LogManager::LogManager(const std::string& wal_file,
                       const DatabaseId& database_id,
                       bool allow_create_if_missing,
                       bool allow_repair_incomplete_header)
    : wal_file_(wal_file), database_id_(database_id) {
    file_ = std::fopen(wal_file_.c_str(), "r+b");
    if (!file_ && allow_create_if_missing)
        file_ = std::fopen(wal_file_.c_str(), "w+b");
    if (!file_)
        throw std::runtime_error("LogManager: cannot open WAL file: " + wal_file_);

    auto fail = [this](const std::string& message) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error("LogManager: " + message);
    };

    uint64_t size = 0;
    if (!FileSize(file_, &size)) fail("cannot determine WAL size");

    if (size == 0) {
        if (!allow_create_if_missing) fail("WAL is empty; refusing to initialize an existing database's log");
        std::vector<uint8_t> header(kMagic, kMagic + sizeof(kMagic));
        PutU32(header, kFormatVersion);
        header.insert(header.end(), database_id_.begin(), database_id_.end());
        PutU32(header, Crc32(header.data(), header.size()));
        if (!Seek(file_, 0) ||
            std::fwrite(header.data(), 1, header.size(), file_) != header.size() ||
            !SyncFileLocked())
            fail("cannot initialize WAL header");
    } else if (size < kFileHeaderSize && allow_repair_incomplete_header) {
        // Bootstrap only enables this for a brand-new empty database or while
        // a durable migration journal proves that no legacy WAL records exist.
        if (!TruncateFile(file_, 0) || !Seek(file_, 0))
            fail("cannot reset incomplete WAL header");
        std::vector<uint8_t> header(kMagic, kMagic + sizeof(kMagic));
        PutU32(header, kFormatVersion);
        header.insert(header.end(), database_id_.begin(), database_id_.end());
        PutU32(header, Crc32(header.data(), header.size()));
        if (std::fwrite(header.data(), 1, header.size(), file_) != header.size() || !SyncFileLocked())
            fail("cannot persist repaired WAL header");
    } else {
        if (size < kFileHeaderSize) fail("truncated WAL file header");
        std::array<uint8_t, kFileHeaderSize> header{};
        if (!Seek(file_, 0) || std::fread(header.data(), 1, header.size(), file_) != header.size())
            fail("cannot read WAL file header");
        if (std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0 ||
            GetU32(header.data() + 8) != kFormatVersion ||
            GetU32(header.data() + 28) != Crc32(header.data(), 28))
            fail("invalid WAL header or unsupported version");
        if (!std::equal(database_id_.begin(), database_id_.end(), header.begin() + 12))
            fail("WAL database identity does not match the database file");
    }

    std::vector<LogRecord> existing;
    if (!ScanLocked(&existing, true)) fail(last_error_);
    if (!Seek(file_, 0) || !FileSize(file_, &size) || !Seek(file_, size))
        fail("cannot seek to WAL end");
}
LogManager::~LogManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool LogManager::SetErrorLocked(const std::string& message) {
    healthy_ = false;
    last_error_ = message;
    return false;
}

bool LogManager::SyncFileLocked() {
    if (!file_ || std::fflush(file_) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file_)) == 0;
#else
    return fsync(fileno(file_)) == 0;
#endif
}

bool LogManager::AppendRecordLocked(LogRecordType type, uint64_t txn_id,
                                    page_id_t page_id,
                                    const std::vector<uint8_t>& payload,
                                    uint64_t* lsn_out) {
    if (!healthy_ || !file_) return false;
    if (!ValidType(static_cast<uint8_t>(type))) return SetErrorLocked("invalid record type");
    if (payload.size() > kMaxPayloadSize) return SetErrorLocked("WAL payload exceeds maximum size");
    if ((type == LogRecordType::PAGE_UPDATE &&
         (payload.size() != 2U * PAGE_SIZE || page_id == INVALID_PAGE_ID)) ||
        (type != LogRecordType::PAGE_UPDATE && !payload.empty()))
        return SetErrorLocked("invalid payload for WAL record type");
    if (last_lsn_ == std::numeric_limits<uint64_t>::max()) return SetErrorLocked("LSN space exhausted");

    const uint64_t lsn = last_lsn_ + 1;
    std::vector<uint8_t> bytes;
    bytes.reserve(kRecordHeaderSize + payload.size());
    bytes.push_back(static_cast<uint8_t>(type));
    bytes.insert(bytes.end(), 3, 0);
    PutU32(bytes, static_cast<uint32_t>(kRecordHeaderSize + payload.size()));
    PutU64(bytes, lsn);
    PutU64(bytes, txn_id);
    PutU32(bytes, static_cast<uint32_t>(page_id));
    PutU32(bytes, static_cast<uint32_t>(payload.size()));
    PutU32(bytes, 0);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const uint32_t crc = Crc32(bytes.data(), bytes.size());
    for (unsigned i = 0; i < 4; ++i) bytes[32 + i] = static_cast<uint8_t>(crc >> (8U * i));

    if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size())
        return SetErrorLocked("failed to append WAL record");
    last_lsn_ = lsn;
    if (lsn_out) *lsn_out = lsn;
    return true;
}

bool LogManager::AppendRecord(LogRecordType type, uint64_t txn_id,
                              page_id_t page_id, const std::vector<uint8_t>& payload,
                              uint64_t* lsn_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    return AppendRecordLocked(type, txn_id, page_id, payload, lsn_out);
}
bool LogManager::AppendBegin(uint64_t txn, uint64_t* lsn) {
    return AppendRecord(LogRecordType::BEGIN, txn, INVALID_PAGE_ID, {}, lsn);
}
bool LogManager::AppendPageUpdate(uint64_t txn, page_id_t page,
                                  const char* before_image, const char* after_image,
                                  uint64_t* lsn) {
    if (!before_image || !after_image) {
        std::lock_guard<std::mutex> lock(mutex_);
        return SetErrorLocked("null page image");
    }
    std::vector<uint8_t> payload(2U * PAGE_SIZE);
    std::memcpy(payload.data(), before_image, PAGE_SIZE);
    std::memcpy(payload.data() + PAGE_SIZE, after_image, PAGE_SIZE);
    return AppendRecord(LogRecordType::PAGE_UPDATE, txn, page, payload, lsn);
}
bool LogManager::AppendCommit(uint64_t txn, uint64_t* lsn) {
    return AppendRecord(LogRecordType::COMMIT, txn, INVALID_PAGE_ID, {}, lsn);
}
bool LogManager::AppendAbort(uint64_t txn, uint64_t* lsn) {
    return AppendRecord(LogRecordType::ABORT, txn, INVALID_PAGE_ID, {}, lsn);
}
bool LogManager::AppendCheckpoint(uint64_t* lsn) {
    return AppendRecord(LogRecordType::CHECKPOINT, 0, INVALID_PAGE_ID, {}, lsn);
}

bool LogManager::ScanLocked(std::vector<LogRecord>* records, bool repair_tail) {
    if (!records || !file_) return SetErrorLocked("invalid scan request");
    records->clear();
    uint64_t size = 0;
    if (!FileSize(file_, &size) || size < kFileHeaderSize)
        return SetErrorLocked("cannot read WAL file size");
    uint64_t offset = kFileHeaderSize;
    uint64_t expected_lsn = 1;
    bool repaired_tail = false;
    while (offset < size) {
        const uint64_t remaining = size - offset;
        if (remaining < kRecordHeaderSize) {
            if (!repair_tail || !TruncateFile(file_, offset)) return SetErrorLocked("incomplete final WAL record");
            size = offset; repaired_tail = true; break;
        }
        std::array<uint8_t, kRecordHeaderSize> h{};
        if (!Seek(file_, offset) || std::fread(h.data(), 1, h.size(), file_) != h.size())
            return SetErrorLocked("failed reading WAL record header");
        const uint8_t raw_type = h[0];
        const uint32_t total_len = GetU32(h.data() + 4);
        const uint64_t lsn = GetU64(h.data() + 8);
        const uint64_t txn = GetU64(h.data() + 16);
        const page_id_t page = static_cast<page_id_t>(GetU32(h.data() + 24));
        const uint32_t payload_len = GetU32(h.data() + 28);
        const uint32_t stored_crc = GetU32(h.data() + 32);
        if (!ValidType(raw_type) || h[1] || h[2] || h[3] ||
            total_len != kRecordHeaderSize + payload_len || payload_len > kMaxPayloadSize ||
            lsn != expected_lsn)
            return SetErrorLocked("malformed WAL record at byte " + std::to_string(offset));
        if (total_len > remaining) {
            if (!repair_tail || !TruncateFile(file_, offset)) return SetErrorLocked("incomplete final WAL record");
            size = offset; repaired_tail = true; break;
        }
        std::vector<uint8_t> bytes(total_len);
        std::memcpy(bytes.data(), h.data(), h.size());
        if (payload_len && std::fread(bytes.data() + kRecordHeaderSize, 1, payload_len, file_) != payload_len)
            return SetErrorLocked("failed reading WAL record payload");
        std::fill(bytes.begin() + 32, bytes.begin() + 36, 0);
        if (Crc32(bytes.data(), bytes.size()) != stored_crc)
            return SetErrorLocked("WAL checksum mismatch at LSN " + std::to_string(lsn));
        const auto type = static_cast<LogRecordType>(raw_type);
        if ((type == LogRecordType::PAGE_UPDATE &&
             (payload_len != 2U * PAGE_SIZE || page == INVALID_PAGE_ID)) ||
            (type != LogRecordType::PAGE_UPDATE && payload_len != 0))
            return SetErrorLocked("invalid WAL payload at LSN " + std::to_string(lsn));
        LogRecord record;
        record.type = type; record.lsn = lsn; record.txn_id = txn; record.page_id = page;
        record.payload.assign(bytes.begin() + kRecordHeaderSize, bytes.end());
        records->push_back(std::move(record));
        ++expected_lsn;
        offset += total_len;
    }
    last_lsn_ = expected_lsn - 1;
    if (repair_tail && repaired_tail && !SyncFileLocked())
        return SetErrorLocked("failed to durably persist WAL tail repair");
    if (repair_tail && size < offset) return SetErrorLocked("failed repairing WAL tail");
    return true;
}

bool LogManager::Scan(std::vector<LogRecord>* records) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!healthy_) return false;
    const bool ok = ScanLocked(records, true);
    if (ok) {
        uint64_t size = 0;
        if (!FileSize(file_, &size) || !Seek(file_, size)) return SetErrorLocked("cannot restore WAL append position");
    }
    return ok;
}

bool LogManager::FlushUpTo(uint64_t lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!healthy_ || !file_) return false;
    if (lsn > last_lsn_) return SetErrorLocked("cannot flush an LSN that has not been appended");
    if (lsn <= durable_lsn_) return true;
    if (!SyncFileLocked()) return SetErrorLocked("operating-system WAL sync failed");
    durable_lsn_ = last_lsn_;
    return true;
}
uint64_t LogManager::GetLastLSN() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_lsn_;
}
std::string LogManager::GetLastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

}  // namespace ocera
