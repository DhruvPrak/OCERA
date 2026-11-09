#include "record/record_serializer.h"

#include <cstddef>
#include <limits>
#include <utility>

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

void AppendU32(
    std::vector<std::uint8_t>* output,
    std::uint32_t value
) {
    for (unsigned int i = 0; i < 4; ++i) {
        output->push_back(static_cast<std::uint8_t>(
            (value >> (8U * i)) & 0xffU
        ));
    }
}

void AppendU64(
    std::vector<std::uint8_t>* output,
    std::uint64_t value
) {
    for (unsigned int i = 0; i < 8; ++i) {
        output->push_back(static_cast<std::uint8_t>(
            (value >> (8U * i)) & 0xffU
        ));
    }
}

bool ReadU32(
    const std::vector<std::uint8_t>& input,
    std::size_t* position,
    std::uint32_t* value
) {
    if (position == nullptr ||
        value == nullptr ||
        *position > input.size() ||
        input.size() - *position < 4) {
        return false;
    }

    const std::size_t p = *position;

    *value =
        static_cast<std::uint32_t>(input[p]) |
        (static_cast<std::uint32_t>(input[p + 1]) << 8U) |
        (static_cast<std::uint32_t>(input[p + 2]) << 16U) |
        (static_cast<std::uint32_t>(input[p + 3]) << 24U);

    *position += 4;
    return true;
}

bool ReadU64(
    const std::vector<std::uint8_t>& input,
    std::size_t* position,
    std::uint64_t* value
) {
    if (position == nullptr ||
        value == nullptr ||
        *position > input.size() ||
        input.size() - *position < 8) {
        return false;
    }

    std::uint64_t result = 0;

    for (unsigned int i = 0; i < 8; ++i) {
        result |=
            static_cast<std::uint64_t>(input[*position + i])
            << (8U * i);
    }

    *position += 8;
    *value = result;
    return true;
}

// Interpret a two's-complement 64-bit bit pattern without relying on an
// out-of-range unsigned-to-signed conversion.
std::int64_t DecodeInt64(std::uint64_t bits) {
    const std::uint64_t max_signed =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()
        );

    if (bits <= max_signed) {
        return static_cast<std::int64_t>(bits);
    }

    // ~bits is guaranteed to be <= INT64_MAX in this branch.
    return -1 - static_cast<std::int64_t>(~bits);
}

bool ValidateSchema(
    const TableSchema& schema,
    std::string* error
) {
    if (schema.columns.empty()) {
        SetError(error, "cannot serialize a table with no columns");
        return false;
    }

    for (const ColumnSchema& column : schema.columns) {
        if (column.type != sql::ColumnType::INT &&
            column.type != sql::ColumnType::TEXT) {
            SetError(
                error,
                "unsupported type for column '" + column.name + "'"
            );
            return false;
        }
    }

    return true;
}

}  // namespace

bool RecordSerializer::Encode(
    const TableSchema& schema,
    const std::vector<sql::Value>& values,
    std::vector<std::uint8_t>* output,
    std::string* error
) {
    ClearError(error);

    if (output == nullptr) {
        SetError(error, "output pointer is null");
        return false;
    }

    if (!ValidateSchema(schema, error)) {
        return false;
    }

    if (values.size() != schema.columns.size()) {
        SetError(
            error,
            "value count does not match schema column count"
        );
        return false;
    }

    // Build separately so failure never partially changes caller output.
    std::vector<std::uint8_t> encoded;

    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        const ColumnSchema& column = schema.columns[i];
        const sql::Value& value = values[i];

        if (column.type == sql::ColumnType::INT) {
            if (value.type != sql::Value::Type::INTEGER) {
                SetError(
                    error,
                    "column '" + column.name +
                    "' expects an integer value"
                );
                return false;
            }

            // Conversion to uint64_t is defined modulo 2^64.
            // The byte representation is signed two's complement.
            AppendU64(
                &encoded,
                static_cast<std::uint64_t>(value.int_value)
            );
        } else if (column.type == sql::ColumnType::TEXT) {
            if (value.type != sql::Value::Type::STRING) {
                SetError(
                    error,
                    "column '" + column.name +
                    "' expects a string value"
                );
                return false;
            }

            if (value.string_value.size() >
                std::numeric_limits<std::uint32_t>::max()) {
                SetError(
                    error,
                    "string value is too large for column '" +
                    column.name + "'"
                );
                return false;
            }

            const auto length = static_cast<std::uint32_t>(
                value.string_value.size()
            );

            AppendU32(&encoded, length);

            for (unsigned char byte : value.string_value) {
                encoded.push_back(static_cast<std::uint8_t>(byte));
            }
        }
    }

    output->swap(encoded);
    return true;
}

bool RecordSerializer::Decode(
    const TableSchema& schema,
    const std::vector<std::uint8_t>& input,
    std::vector<sql::Value>* output,
    std::string* error
) {
    ClearError(error);

    if (output == nullptr) {
        SetError(error, "output pointer is null");
        return false;
    }

    if (!ValidateSchema(schema, error)) {
        return false;
    }

    std::vector<sql::Value> decoded;
    decoded.reserve(schema.columns.size());

    std::size_t position = 0;

    for (const ColumnSchema& column : schema.columns) {
        sql::Value value;

        if (column.type == sql::ColumnType::INT) {
            std::uint64_t bits = 0;

            if (!ReadU64(input, &position, &bits)) {
                SetError(
                    error,
                    "truncated integer value for column '" +
                    column.name + "'"
                );
                return false;
            }

            value.type = sql::Value::Type::INTEGER;
            value.int_value = DecodeInt64(bits);
        } else if (column.type == sql::ColumnType::TEXT) {
            std::uint32_t length = 0;

            if (!ReadU32(input, &position, &length)) {
                SetError(
                    error,
                    "truncated text length for column '" +
                    column.name + "'"
                );
                return false;
            }

            if (position > input.size() ||
                static_cast<std::size_t>(length) >
                    input.size() - position) {
                SetError(
                    error,
                    "truncated text data for column '" +
                    column.name + "'"
                );
                return false;
            }

            value.type = sql::Value::Type::STRING;
            value.string_value.assign(
                input.begin() + static_cast<std::ptrdiff_t>(position),
                input.begin() + static_cast<std::ptrdiff_t>(
                    position + static_cast<std::size_t>(length)
                )
            );

            position += static_cast<std::size_t>(length);
        }

        decoded.push_back(std::move(value));
    }

    if (position != input.size()) {
        SetError(error, "record contains trailing bytes");
        return false;
    }

    output->swap(decoded);
    return true;
}

}  // namespace ocera