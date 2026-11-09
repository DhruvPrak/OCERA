#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ocera {
namespace sql {

enum class ColumnType { INT, TEXT };

struct Value {
    enum class Type { INTEGER, STRING };
    Type type = Type::INTEGER;
    std::int64_t int_value = 0;
    std::string string_value;
    std::size_t position = 0;
};

enum class CompareOp { EQ, NE, LT, GT, LE, GE };
enum class LogicalOp { AND, OR };

struct Comparison {
    std::string column;
    CompareOp op = CompareOp::EQ;
    Value value;
    std::size_t position = 0;
};

struct Condition {
    std::vector<Comparison> comparisons;
    std::vector<LogicalOp> connectors;
};

struct ColumnDefinition {
    std::string name;
    ColumnType type = ColumnType::INT;
    std::size_t position = 0;
};

enum class KeyConstraintType { PRIMARY_KEY, UNIQUE };

struct KeyConstraint {
    KeyConstraintType type = KeyConstraintType::UNIQUE;
    std::vector<std::string> columns;
    std::size_t position = 0;
};

struct ForeignKeyConstraint {
    std::vector<std::string> columns;
    std::string referenced_table;
    std::vector<std::string> referenced_columns;
    std::size_t position = 0;
};

struct Assignment {
    std::string column;
    Value value;
    std::size_t position = 0;
};

struct CreateTableStatement {
    std::string table;
    std::vector<ColumnDefinition> columns;
    std::vector<KeyConstraint> key_constraints;
    std::vector<ForeignKeyConstraint> foreign_key_constraints;
};

struct CreateIndexStatement {
    std::string index_name;
    std::string table;
    std::string column;
};

struct InsertStatement {
    std::string table;
    std::vector<Value> values;
};

struct SelectStatement {
    std::string table;
    bool select_all = false;
    std::vector<std::string> columns;
    std::optional<Condition> where;
};

struct UpdateStatement {
    std::string table;
    std::vector<Assignment> assignments;
    std::optional<Condition> where;
};

struct DeleteStatement {
    std::string table;
    std::optional<Condition> where;
};

using Statement = std::variant<CreateTableStatement, CreateIndexStatement,
                               InsertStatement, SelectStatement,
                               UpdateStatement, DeleteStatement>;

}  // namespace sql
}  // namespace ocera
