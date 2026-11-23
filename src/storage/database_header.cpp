#include "database_header.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef CREATE_NEW
#undef OPEN_EXISTING
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ocera {
namespace {
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kPageSizeOffset = 8;
constexpr std::size_t kCatalogRootOffset = 12;
constexpr std::size_t kDatabaseIdOffset = 16;
constexpr std::size_t kHeaderCrcOffset = 32;
constexpr std::uint32_t kLegacyVersion = 1;
constexpr std::size_t kUpgradeFileSize = 8 + 4 + PAGE_SIZE + 4;
constexpr char kUpgradeMagic[8] = {'O','C','E','R','A','U','P','G'};

void WriteU32LE(char* buf, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        buf[offset + i] = static_cast<char>((value >> (8U * i)) & 0xFFU);
}
std::uint32_t ReadU32LE(const char* buf, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + i])) << (8U * i);
    return value;
}
std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
DatabaseId GenerateDatabaseId() {
    std::random_device random;
    DatabaseId id{};
    for (auto& byte : id) byte = static_cast<std::uint8_t>(random());
    id[6] = static_cast<std::uint8_t>((id[6] & 0x0FU) | 0x40U);
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3FU) | 0x80U);
    return id;
}
void BuildV2Header(char* page, const DatabaseId& id, page_id_t root) {
    std::memset(page, 0, PAGE_SIZE);
    std::memcpy(page + kMagicOffset, DatabaseHeader::kMagic, sizeof(DatabaseHeader::kMagic));
    WriteU32LE(page, kVersionOffset, DatabaseHeader::kCurrentVersion);
    WriteU32LE(page, kPageSizeOffset, static_cast<std::uint32_t>(PAGE_SIZE));
    WriteU32LE(page, kCatalogRootOffset, static_cast<std::uint32_t>(root));
    std::memcpy(page + kDatabaseIdOffset, id.data(), id.size());
    WriteU32LE(page, kHeaderCrcOffset,
               Crc32(reinterpret_cast<const std::uint8_t*>(page), kHeaderCrcOffset));
}
void ValidateV2Header(const char* page) {
    if (std::memcmp(page, DatabaseHeader::kMagic, sizeof(DatabaseHeader::kMagic)) != 0)
        throw std::runtime_error("DatabaseHeader: database magic mismatch");
    const auto version = ReadU32LE(page, kVersionOffset);
    if (version != DatabaseHeader::kCurrentVersion)
        throw std::runtime_error("DatabaseHeader: unsupported database format version " + std::to_string(version));
    const auto page_size = ReadU32LE(page, kPageSizeOffset);
    if (page_size != PAGE_SIZE)
        throw std::runtime_error("DatabaseHeader: database page size mismatch");
    if (ReadU32LE(page, kHeaderCrcOffset) !=
        Crc32(reinterpret_cast<const std::uint8_t*>(page), kHeaderCrcOffset))
        throw std::runtime_error("DatabaseHeader: header checksum mismatch");
    bool all_zero = true;
    for (std::size_t i = 0; i < 16; ++i) all_zero = all_zero && page[kDatabaseIdOffset + i] == 0;
    if (all_zero) throw std::runtime_error("DatabaseHeader: database identity is invalid");
}
void SyncPath(const std::string& path) {
#if defined(_WIN32)
    const int fd = _open(path.c_str(), _O_RDWR | _O_BINARY);
    if (fd < 0) throw std::runtime_error("DatabaseHeader: cannot open migration journal for sync");
    const int result = _commit(fd);
    const int close_result = _close(fd);
#else
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) throw std::runtime_error("DatabaseHeader: cannot open migration journal for sync");
    const int result = ::fsync(fd);
    const int close_result = ::close(fd);
#endif
    if (result != 0 || close_result != 0)
        throw std::runtime_error("DatabaseHeader: migration journal sync failed");
}
void SyncDirectory(const std::filesystem::path& path) {
#if !defined(_WIN32)
    const auto directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int fd = ::open(directory.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("DatabaseHeader: cannot open directory for sync");
    const int result = ::fsync(fd);
    const int close_result = ::close(fd);
    if (result != 0 || close_result != 0)
        throw std::runtime_error("DatabaseHeader: directory sync failed");
#else
    (void)path;
    // The journal file itself is committed with _commit. Windows does not
    // expose a portable directory-fsync equivalent through the CRT.
#endif
}
std::string UpgradePath(const std::string& database_file) { return database_file + ".upgrade"; }
std::vector<char> ReadUpgradePage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("DatabaseHeader: cannot open migration journal");
    std::vector<std::uint8_t> bytes(kUpgradeFileSize);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (in.gcount() != static_cast<std::streamsize>(bytes.size()) || in.peek() != EOF)
        throw std::runtime_error("DatabaseHeader: malformed migration journal length");
    if (std::memcmp(bytes.data(), kUpgradeMagic, sizeof(kUpgradeMagic)) != 0 ||
        ReadU32LE(reinterpret_cast<const char*>(bytes.data()), 8) != 1 ||
        Crc32(bytes.data(), 8 + 4 + PAGE_SIZE) != ReadU32LE(reinterpret_cast<const char*>(bytes.data()), 8 + 4 + PAGE_SIZE))
        throw std::runtime_error("DatabaseHeader: migration journal checksum or format is invalid");
    std::vector<char> page(PAGE_SIZE);
    std::memcpy(page.data(), bytes.data() + 12, PAGE_SIZE);
    ValidateV2Header(page.data());
    return page;
}
void WriteUpgradeJournal(const std::string& path, const char* target_page) {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), kUpgradeMagic, kUpgradeMagic + sizeof(kUpgradeMagic));
    bytes.push_back(1); bytes.push_back(0); bytes.push_back(0); bytes.push_back(0);
    bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(target_page),
                 reinterpret_cast<const std::uint8_t*>(target_page) + PAGE_SIZE);
    const auto crc = Crc32(bytes.data(), bytes.size());
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(crc >> (8U * i)));
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("DatabaseHeader: cannot write migration journal");
        out.flush();
        if (!out) throw std::runtime_error("DatabaseHeader: migration journal write failed");
    }
    SyncPath(temp);
#if defined(_WIN32)
    if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("DatabaseHeader: cannot durably publish migration journal");
#else
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) throw std::runtime_error("DatabaseHeader: cannot publish migration journal: " + ec.message());
    SyncDirectory(std::filesystem::path(path));
#endif
}
void CheckLegacyWal(const std::string& wal_file) {
    if (!std::filesystem::exists(wal_file)) return;
    const auto size = std::filesystem::file_size(wal_file);
    if (size == 0) return;
    if (size < 16)
        throw std::runtime_error("DatabaseHeader: existing WAL has a truncated header");
    std::array<std::uint8_t, 16> header{};
    std::ifstream in(wal_file, std::ios::binary);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const char magic[8] = {'O','C','E','R','A','W','A','L'};
    if (!in || std::memcmp(header.data(), magic, 8) != 0 ||
        ReadU32LE(reinterpret_cast<const char*>(header.data()), 12) != Crc32(header.data(), 12))
        throw std::runtime_error("DatabaseHeader: existing WAL header is invalid; refusing database migration");
    const auto version = ReadU32LE(reinterpret_cast<const char*>(header.data()), 8);
    if (version != 1)
        throw std::runtime_error("DatabaseHeader: existing WAL is not a legacy v1 log and cannot be associated with a v1 database");
    if (size != 16)
        throw std::runtime_error("DatabaseHeader: legacy WAL contains transaction records but has no database identity; preserve it and resolve it before migration");
}
}  // namespace

DatabaseHeader::DatabaseHeader(DiskManager& disk_manager) : disk_manager_(disk_manager) {
    if (disk_manager_.GetNumPages() <= HEADER_PAGE_ID) WriteDefaults();
    else Load();
}

DatabaseHeader::BootstrapResult DatabaseHeader::Bootstrap(
    DiskManager& disk_manager, const std::string& database_file, const std::string& wal_file) {
    BootstrapResult result;
    const auto marker = UpgradePath(database_file);
#if defined(_WIN32)
    const std::string completed_marker = marker + ".done";
    if (std::filesystem::exists(completed_marker)) DeleteFileA(completed_marker.c_str());
#endif
    const bool marker_exists = std::filesystem::exists(marker);

    if (disk_manager.GetNumPages() == 0) {
        if (std::filesystem::exists(wal_file))
            throw std::runtime_error("DatabaseHeader: an orphan WAL already exists for a new database path; preserve or rename it before creating this database");
        std::array<char, PAGE_SIZE> bitmap{};
        bitmap[BITMAP_PAGE_ID / 8] |= static_cast<char>(1U << (BITMAP_PAGE_ID % 8));
        bitmap[HEADER_PAGE_ID / 8] |= static_cast<char>(1U << (HEADER_PAGE_ID % 8));
        disk_manager.WritePage(BITMAP_PAGE_ID, bitmap.data());
        result.database_id = GenerateDatabaseId();
        std::array<char, PAGE_SIZE> header{};
        BuildV2Header(header.data(), result.database_id, INVALID_PAGE_ID);
        disk_manager.WritePage(HEADER_PAGE_ID, header.data());
        disk_manager.Sync();
        result.allow_wal_creation = true;
        result.allow_wal_repair = true;
        return result;
    }

    if (marker_exists) {
        if (disk_manager.GetNumPages() <= HEADER_PAGE_ID)
            throw std::runtime_error("DatabaseHeader: migration journal exists but database header page is missing");
        const auto target = ReadUpgradePage(marker);
        std::memcpy(result.database_id.data(), target.data() + kDatabaseIdOffset, result.database_id.size());
        disk_manager.WritePage(HEADER_PAGE_ID, target.data());
        disk_manager.Sync();
        result.allow_wal_creation = true;
        result.allow_wal_repair = true;
        result.migration_pending = true;
        return result;
    }

    if (disk_manager.GetNumPages() <= HEADER_PAGE_ID) {
        if (disk_manager.GetNumPages() == 1) {
            std::array<char, PAGE_SIZE> bitmap{};
            disk_manager.ReadPage(BITMAP_PAGE_ID, bitmap.data());
            const bool reserved =
                (static_cast<std::uint8_t>(bitmap[0]) & 0x03U) == 0x03U;
            if (!reserved) throw std::runtime_error("DatabaseHeader: incomplete database has no valid reserved-page bitmap");
            result.database_id = GenerateDatabaseId();
            std::array<char, PAGE_SIZE> header{};
            BuildV2Header(header.data(), result.database_id, INVALID_PAGE_ID);
            disk_manager.WritePage(HEADER_PAGE_ID, header.data());
            disk_manager.Sync();
            result.allow_wal_creation = true;
            result.allow_wal_repair = true;
            return result;
        }
        throw std::runtime_error("DatabaseHeader: database file is incomplete");
    }

    std::array<char, PAGE_SIZE> current{};
    disk_manager.ReadPage(HEADER_PAGE_ID, current.data());
    if (std::memcmp(current.data(), kMagic, sizeof(kMagic)) != 0)
        throw std::runtime_error("DatabaseHeader: not a valid OCERA database (magic mismatch)");
    const auto version = ReadU32LE(current.data(), kVersionOffset);
    if (version == kCurrentVersion) {
        ValidateV2Header(current.data());
        std::memcpy(result.database_id.data(), current.data() + kDatabaseIdOffset, result.database_id.size());
        result.allow_wal_creation = disk_manager.GetNumPages() == 2 &&
            static_cast<page_id_t>(ReadU32LE(current.data(), kCatalogRootOffset)) == INVALID_PAGE_ID;
        result.allow_wal_repair = result.allow_wal_creation;
        return result;
    }
    if (version != kLegacyVersion)
        throw std::runtime_error("DatabaseHeader: unsupported database format version " + std::to_string(version));
    if (ReadU32LE(current.data(), kPageSizeOffset) != PAGE_SIZE)
        throw std::runtime_error("DatabaseHeader: legacy database page size mismatch");

    CheckLegacyWal(wal_file);
    result.database_id = GenerateDatabaseId();
    const auto root = static_cast<page_id_t>(ReadU32LE(current.data(), kCatalogRootOffset));
    std::array<char, PAGE_SIZE> target{};
    BuildV2Header(target.data(), result.database_id, root);
    WriteUpgradeJournal(marker, target.data());
    disk_manager.WritePage(HEADER_PAGE_ID, target.data());
    disk_manager.Sync();
    result.allow_wal_creation = true;
    result.allow_wal_repair = true;
    result.migration_pending = true;
    return result;
}

void DatabaseHeader::CompleteBootstrap(const std::string& database_file) {
    const auto marker = UpgradePath(database_file);
    if (!std::filesystem::exists(marker)) return;
#if defined(_WIN32)
    const std::string completed_marker = marker + ".done";
    if (!MoveFileExA(marker.c_str(), completed_marker.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("DatabaseHeader: cannot durably retire migration journal");
    // A leftover .done file is harmless: the database header and WAL are
    // already durable, and the canonical .upgrade marker has been retired.
    DeleteFileA(completed_marker.c_str());
#else
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    if (ec) throw std::runtime_error("DatabaseHeader: cannot remove completed migration journal: " + ec.message());
    SyncDirectory(std::filesystem::path(marker));
#endif
}

void DatabaseHeader::WriteDefaults() {
    version_ = kCurrentVersion;
    page_size_ = static_cast<std::uint32_t>(PAGE_SIZE);
    catalog_root_page_id_ = INVALID_PAGE_ID;
    database_id_ = GenerateDatabaseId();
    Flush();
}

void DatabaseHeader::Flush() const {
    std::array<char, PAGE_SIZE> page{};
    BuildV2Header(page.data(), database_id_, catalog_root_page_id_);
    disk_manager_.WritePage(HEADER_PAGE_ID, page.data());
}

void DatabaseHeader::SetCatalogRootPageId(page_id_t page_id) {
    if (page_id != INVALID_PAGE_ID && page_id < 2)
        throw std::invalid_argument("catalog root page ID is invalid");
    const page_id_t previous = catalog_root_page_id_;
    catalog_root_page_id_ = page_id;
    try { Flush(); } catch (...) { catalog_root_page_id_ = previous; throw; }
}

void DatabaseHeader::Load() {
    std::array<char, PAGE_SIZE> page{};
    disk_manager_.ReadPage(HEADER_PAGE_ID, page.data());
    ValidateV2Header(page.data());
    version_ = ReadU32LE(page.data(), kVersionOffset);
    page_size_ = ReadU32LE(page.data(), kPageSizeOffset);
    catalog_root_page_id_ = static_cast<page_id_t>(ReadU32LE(page.data(), kCatalogRootOffset));
    std::memcpy(database_id_.data(), page.data() + kDatabaseIdOffset, database_id_.size());
}

}  // namespace ocera
