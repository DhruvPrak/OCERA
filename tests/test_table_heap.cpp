#include "record/table_heap.h"

#include "storage/disk_manager.h"
#include "storage/free_space_manager.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ocera;

namespace {

std::vector<std::uint8_t> MakeRecord(
    std::size_t size,
    std::uint8_t value
) {
    return std::vector<std::uint8_t>(size, value);
}

void CheckRecord(
    TableHeap& heap,
    const RID& rid,
    const std::vector<std::uint8_t>& expected
) {
    std::vector<std::uint8_t> actual;
    assert(heap.Get(rid, &actual));
    assert(actual == expected);
}

}  // namespace

int main() {
    const std::string db_file = "test_table_heap.db";
    std::remove(db_file.c_str());

    page_id_t first_page_id = INVALID_PAGE_ID;

    std::vector<RID> rids;
    std::vector<std::vector<std::uint8_t>> records;

    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        BufferPoolManager bpm(
            3,
            disk_manager,
            free_space_manager
        );

        std::cout << "[1] Creating TableHeap...\n";

        assert(TableHeap::Create(bpm, &first_page_id));
        assert(first_page_id >= 2);

        TableHeap heap(bpm, first_page_id);

        std::cout << "[2] Inserting records across pages...\n";

        for (std::uint8_t i = 1; i <= 12; ++i) {
            auto record = MakeRecord(1000, i);
            RID rid;

            assert(heap.Insert(record, &rid));

            records.push_back(record);
            rids.push_back(rid);
        }

        bool multiple_pages_used = false;

        for (const RID& rid : rids) {
            if (rid.page_id != first_page_id) {
                multiple_pages_used = true;
                break;
            }
        }

        assert(multiple_pages_used);

        std::cout << "[3] Retrieving records...\n";

        for (std::size_t i = 0; i < rids.size(); ++i) {
            CheckRecord(heap, rids[i], records[i]);
        }

        std::cout << "[4] Updating a record...\n";

        const auto updated = MakeRecord(250, 0xAA);

        assert(heap.Update(rids[0], updated));

        records[0] = updated;
        CheckRecord(heap, rids[0], updated);

        std::cout << "[5] Deleting and reusing a slot...\n";

        const RID deleted_rid = rids[1];

        assert(heap.Delete(deleted_rid));

        std::vector<std::uint8_t> output;
        assert(!heap.Get(deleted_rid, &output));

        const auto replacement = MakeRecord(500, 0xBB);
        RID replacement_rid;

        assert(heap.Insert(replacement, &replacement_rid));

        assert(replacement_rid.page_id == deleted_rid.page_id);
        assert(replacement_rid.slot_num == deleted_rid.slot_num);
        assert(replacement_rid.generation >
               deleted_rid.generation);

        assert(!heap.Get(deleted_rid, &output));

        CheckRecord(heap, replacement_rid, replacement);

        records[1] = replacement;
        rids[1] = replacement_rid;

        std::cout << "[6] Flushing pages...\n";
        bpm.FlushAllPages();
    }

    {
        std::cout << "[7] Reopening database...\n";

        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        BufferPoolManager bpm(
            3,
            disk_manager,
            free_space_manager
        );

        TableHeap heap(bpm, first_page_id);

        for (std::size_t i = 0; i < rids.size(); ++i) {
            CheckRecord(heap, rids[i], records[i]);
        }

        std::cout << "All surviving records persisted.\n";

        bpm.FlushAllPages();
    }

    std::remove(db_file.c_str());

    std::cout << "\nAll TableHeap tests passed.\n";

    return 0;
}