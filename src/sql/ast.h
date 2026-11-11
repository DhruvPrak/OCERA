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
    enum class Type { INTEGER, STRING, NULL_VALUE };
    Type type = Type::INTEGER;
    std::int64_t int_value = 0;
    std::string string_value;
    std::size_t position = 0;
};

enum class CompareOp { EQ, NE, LT, GT, LE, GE };
enum class LogicalOp { AND, OR };
enum class PredicateType { COMPARISON, IS_NULL, LIKE, IN, BETWEEN };

struct Comparison {
    std::string column;
    CompareOp op = CompareOp::EQ;
    Value value;
    std::size_t position = 0;
};

struct ConditionPredicate {
    PredicateType type = PredicateType::COMPARISON;
    std::string column;
    CompareOp op = CompareOp::EQ;
    std::vector<Value> values;
    bool negated = false;
    std::size_t position = 0;
};

struct Condition {
    // `predicates` is the complete ordered predicate list used by validation
    // and execution. `comparisons` remains populated for ordinary comparisons
    // to preserve the existing public AST inspection contract.
    std::vector<ConditionPredicate> predicates;
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

enum class JoinType { INNER, LEFT, RIGHT, FULL_OUTER, CROSS };

// The first implementation deliberately supports a single equality between
// two column references in JOIN ... ON. More general expressions can be added
// without changing the join tree representation.
struct JoinCondition {
    std::string left_column;
    std::string right_column;
    std::size_t position = 0;
};

struct JoinClause {
    JoinType type = JoinType::INNER;
    std::string table;
    std::optional<JoinCondition> on;
    std::size_t position = 0;
};

struct OrderByItem {
    std::string column;
    bool descending = false;
    std::size_t position = 0;
};

struct SelectStatement {
    std::string table;
    bool select_all = false;
    bool distinct = false;
    std::vector<std::string> columns;
    std::vector<JoinClause> joins;
    std::optional<Condition> where;
    std::vector<OrderByItem> order_by;
    std::optional<std::size_t> limit;
    std::size_t offset = 0;
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

struct ShowTablesStatement {};

struct DescTableStatement {
    std::string table;
};

struct DropTableStatement {
    std::string table;
};

using Statement = std::variant<CreateTableStatement, CreateIndexStatement,
                               InsertStatement, SelectStatement,
                               UpdateStatement, DeleteStatement,
                               ShowTablesStatement, DescTableStatement,
                               DropTableStatement>;

}  // namespace sql
}  // namespace ocera
