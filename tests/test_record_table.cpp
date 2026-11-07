#include "record/record_table.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/free_space_manager.h"

using namespace ocera;

namespace {

sql::Value Int(std::int64_t number) {
    sql::Value value;
    value.type = sql::Value::Type::INTEGER;
    value.int_value = number;
    return value;
}

sql::Value Text(const std::string& text) {
    sql::Value value;
    value.type = sql::Value::Type::STRING;
    value.string_value = text;
    return value;
}

TableSchema MakeSchema() {
    TableSchema schema;
    schema.name = "students";
    schema.columns = {
        {"id", sql::ColumnType::INT},
        {"name", sql::ColumnType::TEXT},
        {"age", sql::ColumnType::INT},
        {"note", sql::ColumnType::TEXT}
    };
    return schema;
}

void CheckRow(
    RecordTable& table,
    const RID& rid,
    const std::vector<sql::Value>& expected
) {
    std::vector<sql::Value> actual;
    std::string error;
    assert(table.Get(rid, &actual, &error));
    assert(actual.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(actual[i].type == expected[i].type);
        if (expected[i].type == sql::Value::Type::INTEGER) {
            assert(actual[i].int_value == expected[i].int_value);
        } else {
            assert(actual[i].string_value == expected[i].string_value);
        }
    }
}

}  // namespace

int main() {
    const std::string db_file = "test_record_table.db";
    std::remove(db_file.c_str());

    const TableSchema schema = MakeSchema();
    page_id_t first_page_id = INVALID_PAGE_ID;
    RID student_rid;
    RID malformed_rid;

    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        BufferPoolManager bpm(3, disk_manager, free_space_manager);

        std::cout << "[1] Create heap and typed table adapter\n";
        assert(TableHeap::Create(bpm, &first_page_id));
        TableHeap heap(bpm, first_page_id);
        RecordTable table(schema, heap);

        std::cout << "[2] Insert and retrieve a typed row\n";
        const std::vector<sql::Value> student = {
            Int(7), Text("Asha"), Int(20), Text("CS")
        };
        std::string error;
        assert(table.Insert(student, &student_rid, &error));
        assert(student_rid.page_id >= 2);
        CheckRow(table, student_rid, student);

        std::cout << "[3] Reject schema/type mismatch without inserting\n";
        RID rejected_rid;
        assert(!table.Insert(
            {Text("wrong type"), Text("Asha"), Int(20), Text("CS")},
            &rejected_rid,
            &error
        ));
        assert(rejected_rid.page_id == INVALID_PAGE_ID);
        assert(!error.empty());
        CheckRow(table, student_rid, student);

        std::cout << "[4] Update a row using typed values\n";
        const std::vector<sql::Value> updated = {
            Int(7), Text("Asha Singh"), Int(21), Text("Database systems")
        };
        assert(table.Update(student_rid, updated, &error));
        CheckRow(table, student_rid, updated);

        std::cout << "[5] Failed typed update leaves the stored row unchanged\n";
        assert(!table.Update(
            student_rid,
            {Int(7), Int(123), Int(21), Text("bad name type")},
            &error
        ));
        CheckRow(table, student_rid, updated);

        std::cout << "[6] Malformed stored bytes do not overwrite caller output\n";
        assert(heap.Insert(std::vector<std::uint8_t>{0x01}, &malformed_rid));
        std::vector<sql::Value> preserved = {Int(999)};
        assert(!table.Get(malformed_rid, &preserved, &error));
        assert(preserved.size() == 1);
        assert(preserved[0].type == sql::Value::Type::INTEGER);
        assert(preserved[0].int_value == 999);

        std::cout << "[7] Delete and reject stale RID\n";
        assert(table.Delete(malformed_rid));
        assert(!table.Get(malformed_rid, &preserved, &error));

        bpm.FlushAllPages();
    }

    {
        std::cout << "[8] Reopen heap and decode persisted typed row\n";
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        BufferPoolManager bpm(3, disk_manager, free_space_manager);

        TableHeap heap(bpm, first_page_id);
        RecordTable table(schema, heap);

        const std::vector<sql::Value> expected = {
            Int(7), Text("Asha Singh"), Int(21), Text("Database systems")
        };
        CheckRow(table, student_rid, expected);
        bpm.FlushAllPages();
    }

    std::remove(db_file.c_str());
    std::cout << "\nAll RecordTable integration tests passed.\n";
    return 0;
}
