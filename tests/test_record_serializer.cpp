#include "record/record_serializer.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

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

bool EncodeFails(
    const TableSchema& schema,
    const std::vector<sql::Value>& values
) {
    std::vector<std::uint8_t> output;
    std::string error;
    return !RecordSerializer::Encode(schema, values, &output, &error);
}

bool DecodeFails(
    const TableSchema& schema,
    const std::vector<std::uint8_t>& input
) {
    std::vector<sql::Value> output;
    std::string error;
    return !RecordSerializer::Decode(schema, input, &output, &error);
}

}  // namespace

int main() {
    const TableSchema schema = MakeSchema();

    std::cout << "[1] Mixed INT/TEXT round trip\n";
    {
        const std::vector<sql::Value> original = {
            Int(42),
            Text("Asha"),
            Int(-19),
            Text("database engine")
        };

        std::vector<std::uint8_t> encoded;
        std::string error;

        assert(RecordSerializer::Encode(
            schema, original, &encoded, &error
        ));
        assert(!encoded.empty());

        std::vector<sql::Value> decoded;

        assert(RecordSerializer::Decode(
            schema, encoded, &decoded, &error
        ));

        assert(decoded.size() == original.size());

        assert(decoded[0].type == sql::Value::Type::INTEGER);
        assert(decoded[0].int_value == 42);

        assert(decoded[1].type == sql::Value::Type::STRING);
        assert(decoded[1].string_value == "Asha");

        assert(decoded[2].type == sql::Value::Type::INTEGER);
        assert(decoded[2].int_value == -19);

        assert(decoded[3].type == sql::Value::Type::STRING);
        assert(decoded[3].string_value == "database engine");
    }

    std::cout << "[2] INT boundary values\n";
    {
        TableSchema int_schema;
        int_schema.name = "numbers";
        int_schema.columns = {
            {"minimum", sql::ColumnType::INT},
            {"maximum", sql::ColumnType::INT},
            {"zero", sql::ColumnType::INT}
        };

        const std::vector<sql::Value> original = {
            Int(std::numeric_limits<std::int64_t>::min()),
            Int(std::numeric_limits<std::int64_t>::max()),
            Int(0)
        };

        std::vector<std::uint8_t> encoded;
        std::vector<sql::Value> decoded;

        assert(RecordSerializer::Encode(
            int_schema, original, &encoded
        ));
        assert(RecordSerializer::Decode(
            int_schema, encoded, &decoded
        ));

        assert(decoded[0].int_value ==
               std::numeric_limits<std::int64_t>::min());
        assert(decoded[1].int_value ==
               std::numeric_limits<std::int64_t>::max());
        assert(decoded[2].int_value == 0);
    }

    std::cout << "[3] Empty and binary-containing TEXT\n";
    {
        const std::string binary_text("A\0B", 3);

        const std::vector<sql::Value> original = {
            Int(1),
            Text(""),
            Int(2),
            Text(binary_text)
        };

        std::vector<std::uint8_t> encoded;
        std::vector<sql::Value> decoded;

        assert(RecordSerializer::Encode(
            schema, original, &encoded
        ));
        assert(RecordSerializer::Decode(
            schema, encoded, &decoded
        ));

        assert(decoded[1].string_value.empty());
        assert(decoded[3].string_value == binary_text);
        assert(decoded[3].string_value.size() == 3);
    }

    std::cout << "[4] Reject invalid value counts and types\n";
    {
        assert(EncodeFails(
            schema,
            {Int(1), Text("only two values")}
        ));

        assert(EncodeFails(
            schema,
            {Text("not an integer"), Text("Asha"),
             Int(20), Text("note")}
        ));

        assert(EncodeFails(
            schema,
            {Int(1), Int(2), Int(20), Text("note")}
        ));
    }

    std::cout << "[5] Reject malformed records\n";
    {
        std::vector<std::uint8_t> encoded;

        assert(RecordSerializer::Encode(
            schema,
            {Int(1), Text("abc"), Int(20), Text("ok")},
            &encoded
        ));

        // Remove one byte from the final field.
        auto truncated = encoded;
        truncated.pop_back();
        assert(DecodeFails(schema, truncated));

        // Append an unexpected byte.
        auto trailing = encoded;
        trailing.push_back(0xFF);
        assert(DecodeFails(schema, trailing));

        // A record too short to contain the first INT.
        assert(DecodeFails(
            schema,
            std::vector<std::uint8_t>{0x01, 0x02, 0x03}
        ));
    }

    std::cout << "[6] Failed operations preserve caller output\n";
    {
        std::vector<std::uint8_t> bytes = {0xAA, 0xBB};
        std::string error;

        assert(!RecordSerializer::Encode(
            schema,
            {Text("wrong"), Text("Asha"), Int(20), Text("note")},
            &bytes,
            &error
        ));

        assert((bytes == std::vector<std::uint8_t>{0xAA, 0xBB}));
        assert(!error.empty());

        std::vector<sql::Value> values = {Int(999)};
        const auto original_size = values.size();

        assert(!RecordSerializer::Decode(
            schema,
            std::vector<std::uint8_t>{0x01},
            &values,
            &error
        ));

        assert(values.size() == original_size);
        assert(values[0].int_value == 999);
    }

    std::cout << "[7] Reject empty schemas and null outputs\n";
    {
        TableSchema empty_schema;

        std::vector<std::uint8_t> bytes;
        std::vector<sql::Value> values;

        assert(!RecordSerializer::Encode(
            empty_schema, {}, &bytes
        ));

        assert(!RecordSerializer::Decode(
            empty_schema, {}, &values
        ));

        assert(!RecordSerializer::Encode(
            schema, {}, nullptr
        ));

        assert(!RecordSerializer::Decode(
            schema, {}, nullptr
        ));
    }

    std::cout << "\nAll RecordSerializer tests passed.\n";
    return 0;
}