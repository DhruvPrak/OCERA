#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <stdexcept>

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
    std::remove(blocked_db.c_str()); std::remove(blocked_wal.c_str());
    std::remove(interrupted_db.c_str()); std::remove(interrupted_wal.c_str());
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
            ocera::FreeSpaceManager fsm(disk);
            ocera::DatabaseHeader header(disk);
            Require(header.GetDatabaseId() == wal.GetDatabaseId(), "fresh database/WAL identity mismatch");
            Require(fsm.AllocatePage() == 2, "fresh bootstrap did not reserve pages 0 and 1");
            disk.Sync();
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
            Require(bootstrap.allow_wal_repair, "empty database did not enable interrupted WAL-header repair");
            ocera::LogManager wal(interrupted_wal, bootstrap.database_id,
                                  bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);
            Require(wal.GetDatabaseId() == interrupted_id, "WAL-header repair changed the database identity");
            ocera::DatabaseHeader::CompleteBootstrap(interrupted_db);
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
        std::cout << "Database bootstrap tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Database bootstrap tests failed: " << e.what() << '\n';
        return 1;
    }
}
