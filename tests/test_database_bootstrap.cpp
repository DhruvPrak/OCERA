#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "../src/storage/database_header.h"
#include "../src/storage/free_space_manager.h"
#include "../src/wal/log_manager.h"

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void WriteU32LE(char* data, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        data[offset + i] = static_cast<char>((value >> (8U * i)) & 0xFFU);
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
}

int main() {
    const std::string fresh_db = "test_database_bootstrap_fresh.db";
    const std::string legacy_db = "test_database_bootstrap_legacy.db";
    const std::string fresh_wal = fresh_db + ".wal";
    const std::string legacy_wal = legacy_db + ".wal";
    const std::string blocked_db = "test_database_bootstrap_blocked.db";
    const std::string blocked_wal = blocked_db + ".wal";
    const std::string interrupted_db = "test_database_bootstrap_interrupted.db";
    const std::string interrupted_wal = interrupted_db + ".wal";
    const std::string established_db = "test_database_bootstrap_established.db";
    const std::string established_wal = established_db + ".wal";
    const std::string marker_only_db = "test_database_bootstrap_marker_only.db";
    const std::string marker_only_wal = marker_only_db + ".wal";
    std::remove(blocked_db.c_str()); std::remove(blocked_wal.c_str());
    std::remove(interrupted_db.c_str()); std::remove(interrupted_wal.c_str());
    std::remove(established_db.c_str()); std::remove(established_wal.c_str());
    std::remove(marker_only_db.c_str()); std::remove(marker_only_wal.c_str());
    for (const auto& base : {fresh_db, interrupted_db, established_db, marker_only_db, blocked_db, legacy_db}) {
        std::remove((base + ".upgrade").c_str());
        std::remove((base + ".upgrade.tmp").c_str());
        std::remove((base + ".upgrade.done").c_str());
    }
    std::remove(fresh_db.c_str()); std::remove(fresh_wal.c_str());
    std::remove(legacy_db.c_str()); std::remove(legacy_wal.c_str());
    std::remove((legacy_db + ".upgrade").c_str());
    std::remove((legacy_db + ".upgrade.tmp").c_str());
    std::remove((legacy_db + ".upgrade.done").c_str());

    try {
        // New database: bootstrap writes bitmap/header before the FSM loads page 0.
        ocera::DatabaseId fresh_id{};
        {
            ocera::DiskManager disk(fresh_db, ocera::DatabaseOpenMode::CREATE_NEW);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, fresh_db, fresh_wal);
            fresh_id = bootstrap.database_id;
            ocera::LogManager wal(fresh_wal, fresh_id, bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            ocera::DatabaseHeader::CompleteBootstrap(fresh_db);
            Require(!std::ifstream(fresh_db + ".upgrade").good(),
                    "fresh bootstrap journal was not retired after WAL open");
            ocera::FreeSpaceManager fsm(disk);
            ocera::DatabaseHeader header(disk);
            Require(header.GetDatabaseId() == wal.GetDatabaseId(), "fresh database/WAL identity mismatch");
            Require(fsm.AllocatePage() == 2, "fresh bootstrap did not reserve pages 0 and 1");
            disk.Sync();
        }

        // Simulate a crash after the bitmap page is written but before the
        // database header page is written; the durable intent must resume it.
        ocera::DatabaseId marker_only_id{};
        for (std::size_t i = 0; i < marker_only_id.size(); ++i)
            marker_only_id[i] = static_cast<std::uint8_t>(i + 1);
        std::array<char, ocera::PAGE_SIZE> target_header{};
        std::memcpy(target_header.data(), "MDB1", 4);
        WriteU32LE(target_header.data(), 4, ocera::DatabaseHeader::kCurrentVersion);
        WriteU32LE(target_header.data(), 8, static_cast<std::uint32_t>(ocera::PAGE_SIZE));
        WriteU32LE(target_header.data(), 12, static_cast<std::uint32_t>(ocera::INVALID_PAGE_ID));
        std::memcpy(target_header.data() + 16, marker_only_id.data(), marker_only_id.size());
        WriteU32LE(target_header.data(), 32,
                   Crc32(reinterpret_cast<const std::uint8_t*>(target_header.data()), 32));
        std::array<std::uint8_t, 12 + ocera::PAGE_SIZE + 4> bootstrap_journal{};
        const char journal_magic[8] = {'O','C','E','R','A','U','P','G'};
        std::memcpy(bootstrap_journal.data(), journal_magic, sizeof(journal_magic));
        WriteU32LE(reinterpret_cast<char*>(bootstrap_journal.data()), 8, 1);
        std::memcpy(bootstrap_journal.data() + 12, target_header.data(), target_header.size());
        WriteU32LE(reinterpret_cast<char*>(bootstrap_journal.data()),
                   12 + ocera::PAGE_SIZE,
                   Crc32(bootstrap_journal.data(), 12 + ocera::PAGE_SIZE));
        {
            ocera::DiskManager disk(marker_only_db, ocera::DatabaseOpenMode::CREATE_NEW);
            std::array<char, ocera::PAGE_SIZE> partial_bitmap{};
            disk.WritePage(ocera::BITMAP_PAGE_ID, partial_bitmap.data());
            disk.Sync();
        }
        {
            std::ofstream marker(marker_only_db + ".upgrade", std::ios::binary | std::ios::trunc);
            marker.write(reinterpret_cast<const char*>(bootstrap_journal.data()),
                         static_cast<std::streamsize>(bootstrap_journal.size()));
            Require(static_cast<bool>(marker), "could not write simulated bootstrap intent");
        }
        {
            ocera::DiskManager disk(marker_only_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, marker_only_db, marker_only_wal);
            Require(bootstrap.database_id == marker_only_id,
                    "marker-only bootstrap did not preserve its preselected database identity");
            Require(bootstrap.allow_wal_creation && bootstrap.allow_wal_repair,
                    "marker-only bootstrap did not authorize WAL initialization");
            ocera::LogManager wal(marker_only_wal, bootstrap.database_id,
                                  bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            ocera::DatabaseHeader::CompleteBootstrap(marker_only_db);
            ocera::FreeSpaceManager fsm(disk);
            Require(fsm.AllocatePage() == 2,
                    "marker-only bootstrap did not reconstruct reserved pages");
        }

        // Simulate a process stopping midway through the first WAL-header write.
        ocera::DatabaseId interrupted_id{};
        {
            ocera::DiskManager disk(interrupted_db, ocera::DatabaseOpenMode::CREATE_NEW);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, interrupted_db, interrupted_wal);
            interrupted_id = bootstrap.database_id;
        }
        {
            std::ofstream partial(interrupted_wal, std::ios::binary | std::ios::trunc);
            const char bytes[] = {'O', 'C', 'E', 'R', 'A'};
            partial.write(bytes, sizeof(bytes));
        }
        {
            ocera::DiskManager disk(interrupted_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, interrupted_db, interrupted_wal);
            Require(bootstrap.allow_wal_repair, "interrupted bootstrap did not enable WAL-header repair");
            Require(std::ifstream(interrupted_db + ".upgrade").good(),
                    "interrupted bootstrap journal was lost before WAL repair");
            ocera::LogManager wal(interrupted_wal, bootstrap.database_id,
                                  bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            Require(wal.GetDatabaseId() == interrupted_id, "WAL-header repair changed the database identity");
            ocera::DatabaseHeader::CompleteBootstrap(interrupted_db);
            Require(!std::ifstream(interrupted_db + ".upgrade").good(),
                    "repaired bootstrap journal was not retired");
        }

        // Once a v2 database has completed bootstrap, neither a missing WAL
        // nor a truncated WAL may be recreated/repaired automatically.
        std::vector<char> established_wal_bytes;
        {
            ocera::DiskManager disk(established_db, ocera::DatabaseOpenMode::CREATE_NEW);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, established_db, established_wal);
            ocera::LogManager wal(established_wal, bootstrap.database_id,
                                  bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            ocera::DatabaseHeader::CompleteBootstrap(established_db);
        }
        {
            std::ifstream in(established_wal, std::ios::binary);
            established_wal_bytes.assign(std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>());
            Require(!established_wal_bytes.empty(), "established WAL was not initialized");
        }
        std::remove(established_wal.c_str());
        {
            ocera::DiskManager disk(established_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, established_db, established_wal);
            Require(!bootstrap.allow_wal_creation && !bootstrap.allow_wal_repair,
                    "established database incorrectly authorized missing-WAL creation");
            bool rejected = false;
            try {
                ocera::LogManager wal(established_wal, bootstrap.database_id,
                                      bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected, "established database silently recreated its missing WAL");
            Require(!std::ifstream(established_wal, std::ios::binary).good(),
                    "failed missing-WAL open unexpectedly created a WAL file");
        }
        {
            std::ofstream out(established_wal, std::ios::binary | std::ios::trunc);
            out.write(established_wal_bytes.data(),
                      static_cast<std::streamsize>(established_wal_bytes.size()));
        }
        {
            std::ofstream out(established_wal, std::ios::binary | std::ios::trunc);
            const char truncated[] = {'O', 'C', 'E', 'R', 'A'};
            out.write(truncated, sizeof(truncated));
        }
        {
            ocera::DiskManager disk(established_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, established_db, established_wal);
            Require(!bootstrap.allow_wal_creation && !bootstrap.allow_wal_repair,
                    "established database incorrectly authorized truncated-WAL repair");
            bool rejected = false;
            try {
                ocera::LogManager wal(established_wal, bootstrap.database_id,
                                      bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            } catch (const std::runtime_error&) { rejected = true; }
            Require(rejected, "established database silently repaired its truncated WAL");
            std::ifstream in(established_wal, std::ios::binary);
            const std::vector<char> remaining((std::istreambuf_iterator<char>(in)),
                                               std::istreambuf_iterator<char>());
            Require(remaining == std::vector<char>({'O', 'C', 'E', 'R', 'A'}),
                    "failed truncated-WAL open modified the existing WAL bytes");
        }

        // Legacy v1 database: migrate only its header and preserve every user page.
        std::array<char, ocera::PAGE_SIZE> old_header{};
        std::memcpy(old_header.data(), "MDB1", 4);
        WriteU32LE(old_header.data(), 4, 1);
        WriteU32LE(old_header.data(), 8, static_cast<std::uint32_t>(ocera::PAGE_SIZE));
        WriteU32LE(old_header.data(), 12, static_cast<std::uint32_t>(ocera::INVALID_PAGE_ID));
        std::array<char, ocera::PAGE_SIZE> payload{};
        std::memcpy(payload.data(), "must-survive-migration", 23);
        {
            ocera::DiskManager disk(legacy_db, ocera::DatabaseOpenMode::CREATE_NEW);
            ocera::FreeSpaceManager fsm(disk);
            disk.WritePage(ocera::HEADER_PAGE_ID, old_header.data());
            const auto page_id = fsm.AllocatePage();
            Require(page_id == 2, "unexpected legacy user page id");
            disk.WritePage(page_id, payload.data());
            disk.Sync();
        }
        // A header-only v1 WAL has no transaction history and can be upgraded.
        {
            std::array<std::uint8_t, 16> old_wal_header{};
            const char wal_magic[8] = {'O','C','E','R','A','W','A','L'};
            std::memcpy(old_wal_header.data(), wal_magic, 8);
            WriteU32LE(reinterpret_cast<char*>(old_wal_header.data()), 8, 1);
            WriteU32LE(reinterpret_cast<char*>(old_wal_header.data()), 12, Crc32(old_wal_header.data(), 12));
            std::ofstream out(legacy_wal, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(old_wal_header.data()), old_wal_header.size());
        }
        {
            ocera::DiskManager disk(legacy_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            const auto bootstrap = ocera::DatabaseHeader::Bootstrap(disk, legacy_db, legacy_wal);
            ocera::LogManager wal(legacy_wal, bootstrap.database_id, bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            ocera::DatabaseHeader::CompleteBootstrap(legacy_db);
            ocera::FreeSpaceManager fsm(disk);
            ocera::DatabaseHeader header(disk);
            std::array<char, ocera::PAGE_SIZE> actual{};
            disk.ReadPage(2, actual.data());
            Require(header.GetVersion() == 2, "legacy header was not upgraded");
            Require(header.GetDatabaseId() == wal.GetDatabaseId(), "migrated database/WAL identity mismatch");
            Require(actual == payload, "legacy user page changed during migration");
            Require(fsm.IsAllocated(2), "legacy user page allocation bit was not preserved");
            disk.Sync();
        }

        // A v1 WAL with any transaction bytes cannot be assigned a database
        // identity safely; migration must fail before changing the v1 header.
        {
            ocera::DiskManager disk(blocked_db, ocera::DatabaseOpenMode::CREATE_NEW);
            ocera::FreeSpaceManager fsm(disk);
            disk.WritePage(ocera::HEADER_PAGE_ID, old_header.data());
            disk.Sync();
        }
        {
            std::array<std::uint8_t, 17> old_wal{};
            const char wal_magic[8] = {'O','C','E','R','A','W','A','L'};
            std::memcpy(old_wal.data(), wal_magic, 8);
            WriteU32LE(reinterpret_cast<char*>(old_wal.data()), 8, 1);
            WriteU32LE(reinterpret_cast<char*>(old_wal.data()), 12, Crc32(old_wal.data(), 12));
            old_wal[16] = 0x01;
            std::ofstream out(blocked_wal, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(old_wal.data()), old_wal.size());
        }
        bool blocked = false;
        try {
            ocera::DiskManager disk(blocked_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            (void)ocera::DatabaseHeader::Bootstrap(disk, blocked_db, blocked_wal);
        } catch (const std::runtime_error&) { blocked = true; }
        Require(blocked, "migration accepted a legacy WAL containing transaction bytes");
        {
            ocera::DiskManager disk(blocked_db, ocera::DatabaseOpenMode::OPEN_EXISTING);
            std::array<char, ocera::PAGE_SIZE> page{};
            disk.ReadPage(ocera::HEADER_PAGE_ID, page.data());
            Require(static_cast<std::uint32_t>(static_cast<unsigned char>(page[4])) == 1,
                    "failed migration modified the legacy database header");
        }

        std::remove(fresh_db.c_str()); std::remove(fresh_wal.c_str());
        std::remove(legacy_db.c_str()); std::remove(legacy_wal.c_str());
        std::remove((legacy_db + ".upgrade").c_str());
        std::remove((legacy_db + ".upgrade.tmp").c_str());
        std::remove((legacy_db + ".upgrade.done").c_str());
        std::remove(blocked_db.c_str()); std::remove(blocked_wal.c_str());
        std::remove(interrupted_db.c_str()); std::remove(interrupted_wal.c_str());
        std::remove(established_db.c_str()); std::remove(established_wal.c_str());
        std::remove(marker_only_db.c_str()); std::remove(marker_only_wal.c_str());
        for (const auto& base : {fresh_db, interrupted_db, established_db, marker_only_db, blocked_db, legacy_db}) {
            std::remove((base + ".upgrade").c_str());
            std::remove((base + ".upgrade.tmp").c_str());
            std::remove((base + ".upgrade.done").c_str());
        }
        std::cout << "Database bootstrap tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Database bootstrap tests failed: " << e.what() << '\n';
        return 1;
    }
}
