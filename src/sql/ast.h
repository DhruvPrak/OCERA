#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ocera {
namespace sql {

// The AST ("abstract syntax tree") is the parser's output: plain data that
// describes WHAT a SQL statement says, without doing anything about it.
// It never looks up tables, touches pages, takes locks or runs queries;
// the executor (a later layer) interprets it.
//
// PROPOSED design: shapes here are not yet approved by the team.

enum class ColumnType { INT, TEXT };

// A literal in SQL text: either an integer (42, -7) or a string ('Asha').
struct Value {
    enum class Type { INTEGER, STRING };
    Type type = Type::INTEGER;
    std::int64_t int_value = 0;  // valid when type == INTEGER
    std::string string_value;    // valid when type == STRING
    std::size_t position = 0;    // where the literal starts in the SQL text
};

enum class CompareOp { EQ, NE, LT, GT, LE, GE };  // =  !=  <  >  <=  >=
enum class LogicalOp { AND, OR };

// One test like  id = 7.
struct Comparison {
    std::string column;
    CompareOp op = CompareOp::EQ;
    Value value;
    std::size_t position = 0;  // where the column name starts
};

// A WHERE clause: comparisons joined by AND/OR, kept as a FLAT list in the
// order written. connectors[i] sits between comparisons[i] and
// comparisons[i+1], so connectors.size() == comparisons.size() - 1.
//
// The parser deliberately does NOT decide precedence (does AND bind tighter
// than OR?). That meaning is left to the executor and must be agreed first.
struct Condition {
    std::vector<Comparison> comparisons;
    std::vector<LogicalOp> connectors;
};

struct ColumnDefinition {
    std::string name;
    ColumnType type = ColumnType::INT;
    std::size_t position = 0;
};

struct Assignment {
    std::string column;
    Value value;
    std::size_t position = 0;
};

// CREATE TABLE students (id INT, name TEXT);
struct CreateTableStatement {
    std::string table;
    std::vector<ColumnDefinition> columns;
};

// INSERT INTO students VALUES (1, 'Asha');
struct InsertStatement {
    std::string table;
    std::vector<Value> values;  // in the order written
};

// SELECT * FROM t;   SELECT a, b FROM t WHERE ...;
struct SelectStatement {
    std::string table;
    bool select_all = false;           // true for '*'
    std::vector<std::string> columns;  // empty when select_all
    std::optional<Condition> where;
};

// UPDATE t SET a = 1, b = 'x' WHERE ...;
struct UpdateStatement {
    std::string table;
    std::vector<Assignment> assignments;  // at least one
    std::optional<Condition> where;
};

// DELETE FROM t WHERE ...;
struct DeleteStatement {
    std::string table;
    std::optional<Condition> where;
};

using Statement = std::variant<CreateTableStatement, InsertStatement,
                               SelectStatement, UpdateStatement,
                               DeleteStatement>;

}  // namespace sql
}  // namespace ocera
