#include "record/record_table.h"

#include <cstdint>
#include <utility>

#include "record/record_serializer.h"

namespace ocera {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void ClearError(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

}  // namespace

RecordTable::RecordTable(
    const TableSchema& schema,
    TableHeap& heap
)
    : schema_(schema),
      heap_(heap) {
}

bool RecordTable::Insert(
    const std::vector<sql::Value>& values,
    RID* rid,
    std::string* error
) {
    ClearError(error);

    if (rid == nullptr) {
        SetError(error, "RID output pointer is null");
        return false;
    }

    *rid = RID{};

    std::vector<std::uint8_t> encoded;
    if (!RecordSerializer::Encode(schema_, values, &encoded, error)) {
        return false;
    }

    if (!heap_.Insert(encoded, rid)) {
        SetError(error, "TableHeap failed to insert the encoded record");
        return false;
    }

    return true;
}

bool RecordTable::Get(
    const RID& rid,
    std::vector<sql::Value>* values,
    std::string* error
) {
    ClearError(error);

    if (values == nullptr) {
        SetError(error, "value output pointer is null");
        return false;
    }

    std::vector<std::uint8_t> encoded;
    if (!heap_.Get(rid, &encoded)) {
        SetError(error, "TableHeap could not retrieve the requested record");
        return false;
    }

    // RecordSerializer builds a temporary decoded vector and only replaces
    // the caller's output on success.
    return RecordSerializer::Decode(schema_, encoded, values, error);
}

bool RecordTable::Update(
    const RID& rid,
    const std::vector<sql::Value>& values,
    std::string* error
) {
    ClearError(error);

    std::vector<std::uint8_t> encoded;
    if (!RecordSerializer::Encode(schema_, values, &encoded, error)) {
        return false;
    }

    if (!heap_.Update(rid, encoded)) {
        SetError(error, "TableHeap failed to update the requested record");
        return false;
    }

    return true;
}

bool RecordTable::Delete(const RID& rid) {
    return heap_.Delete(rid);
}

}  // namespace ocera
