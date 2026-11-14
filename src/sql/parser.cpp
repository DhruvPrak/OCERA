#include "sql/parser.h"
#include <utility>
#include <vector>
#include "sql/tokenizer.h"

namespace ocera {
namespace sql {
namespace {
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Statement ParseOne() {
        Statement stmt = ParseBody();
        if (IsSymbol(Peek(), ";")) Advance();
        if (Peek().type != TokenType::END_OF_INPUT)
            Fail("unexpected " + Describe(Peek()) + " after end of statement",
                 Peek().position);
        return stmt;
    }

private:
    const Token& Peek() const { return tokens_[index_]; }
    const Token& Advance() {
        const Token& t = tokens_[index_];
        if (t.type != TokenType::END_OF_INPUT) ++index_;
        return t;
    }
    [[noreturn]] static void Fail(const std::string& m, std::size_t p) {
        throw ParseError(m, p);
    }
    static std::string Describe(const Token& t) {
        if (t.type == TokenType::END_OF_INPUT) return "end of input";
        if (t.type == TokenType::STRING) return "string '" + t.text + "'";
        return "'" + t.text + "'";
    }
    static bool IsKeyword(const Token& t, const char* kw) {
        return t.type == TokenType::KEYWORD && t.text == kw;
    }
    static bool IsSymbol(const Token& t, const char* sym) {
        return t.type == TokenType::SYMBOL && t.text == sym;
    }
    const Token& ExpectKeyword(const char* kw) {
        if (!IsKeyword(Peek(), kw))
            Fail(std::string("expected ") + kw + " but found " + Describe(Peek()),
                 Peek().position);
        return Advance();
    }
    const Token& ExpectSymbol(const char* sym) {
        if (!IsSymbol(Peek(), sym))
            Fail(std::string("expected '") + sym + "' but found " + Describe(Peek()),
                 Peek().position);
        return Advance();
    }
    const Token& ExpectIdentifier(const char* what) {
        if (Peek().type != TokenType::IDENTIFIER)
            Fail(std::string("expected ") + what + " but found " + Describe(Peek()),
                 Peek().position);
        return Advance();
    }
    std::string ParseOptionalTableAlias() {
        if (IsKeyword(Peek(), "AS")) {
            Advance();
            return ExpectIdentifier("table alias").text;
        }
        if (Peek().type == TokenType::IDENTIFIER)
            return Advance().text;
        return {};
    }
    std::string ParseColumnReference(std::size_t* position = nullptr) {
        const Token& first = ExpectIdentifier("column name");
        if (position) *position = first.position;
        std::string name = first.text;
        while (IsSymbol(Peek(), ".")) {
            Advance();
            name += ".";
            name += ExpectIdentifier("qualified column name component").text;
        }
        return name;
    }

    Statement ParseBody() {
        const Token& first = Peek();
        if (first.type == TokenType::END_OF_INPUT) Fail("empty statement", first.position);
        if (IsKeyword(first, "CREATE")) return ParseCreate();
        if (IsKeyword(first, "INSERT")) return ParseInsert();
        if (IsKeyword(first, "SELECT")) return ParseSelect();
        if (IsKeyword(first, "UPDATE")) return ParseUpdate();
        if (IsKeyword(first, "DELETE")) return ParseDelete();
        if (IsKeyword(first, "SHOW")) return ParseShow();
        if (IsKeyword(first, "DESC")) return ParseDesc();
        if (IsKeyword(first, "DROP")) return ParseDrop();
        Fail("expected CREATE, INSERT, SELECT, UPDATE or DELETE but found " +
             Describe(first), first.position);
    }

    Statement ParseShow() {
    Advance();
    ExpectKeyword("TABLES");
    return ShowTablesStatement{};
}

DescTableStatement ParseDesc() {
    Advance();
    DescTableStatement s;
    s.table = ExpectIdentifier("table name").text;
    return s;
}

DropTableStatement ParseDrop() {
    Advance();
    ExpectKeyword("TABLE");

    DropTableStatement s;
    s.table = ExpectIdentifier("table name").text;
    return s;
}

    Statement ParseCreate() {
        Advance();
        if (IsKeyword(Peek(), "TABLE")) return ParseCreateTable();
        if (IsKeyword(Peek(), "INDEX")) return ParseCreateIndex();
        Fail("expected TABLE or INDEX after CREATE", Peek().position);
    }
    std::vector<std::string> ParseIdentifierList(const char* what) {
        std::vector<std::string> names;
        names.push_back(ExpectIdentifier(what).text);
        while (IsSymbol(Peek(), ",")) {
            Advance();
            names.push_back(ExpectIdentifier(what).text);
        }
        return names;
    }

    CreateTableStatement ParseCreateTable() {
        Advance();
        CreateTableStatement s;
        s.table = ExpectIdentifier("table name").text;
        ExpectSymbol("(");
        while (true) {
            if (IsKeyword(Peek(), "FOREIGN")) {
                ForeignKeyConstraint constraint;
                constraint.position = Peek().position;
                Advance();
                ExpectKeyword("KEY");
                ExpectSymbol("(");
                constraint.columns = ParseIdentifierList("foreign-key column name");
                ExpectSymbol(")");
                ExpectKeyword("REFERENCES");
                constraint.referenced_table = ExpectIdentifier("referenced table name").text;
                ExpectSymbol("(");
                constraint.referenced_columns =
                    ParseIdentifierList("referenced column name");
                ExpectSymbol(")");
                s.foreign_key_constraints.push_back(std::move(constraint));
            } else if (IsKeyword(Peek(), "PRIMARY") || IsKeyword(Peek(), "UNIQUE")) {
                KeyConstraint constraint;
                const Token& kind = Advance();
                constraint.position = kind.position;
                if (kind.text == "PRIMARY") {
                    ExpectKeyword("KEY");
                    constraint.type = KeyConstraintType::PRIMARY_KEY;
                } else {
                    constraint.type = KeyConstraintType::UNIQUE;
                }
                ExpectSymbol("(");
                constraint.columns.push_back(ExpectIdentifier("key column name").text);
                while (IsSymbol(Peek(), ",")) {
                    Advance();
                    constraint.columns.push_back(ExpectIdentifier("key column name").text);
                }
                ExpectSymbol(")");
                s.key_constraints.push_back(std::move(constraint));
            } else {
                ColumnDefinition col;
                const Token& name = ExpectIdentifier("column name");
                col.name = name.text;
                col.position = name.position;
                const Token& type = Peek();
                if (IsKeyword(type, "INT")) col.type = ColumnType::INT;
                else if (IsKeyword(type, "TEXT")) col.type = ColumnType::TEXT;
                else Fail("expected column type INT or TEXT but found " + Describe(type),
                          type.position);
                Advance();
                s.columns.push_back(col);

                if (IsKeyword(Peek(), "PRIMARY")) {
                    KeyConstraint constraint;
                    constraint.type = KeyConstraintType::PRIMARY_KEY;
                    constraint.position = Peek().position;
                    Advance();
                    ExpectKeyword("KEY");
                    constraint.columns.push_back(col.name);
                    s.key_constraints.push_back(std::move(constraint));
                } else if (IsKeyword(Peek(), "UNIQUE")) {
                    KeyConstraint constraint;
                    constraint.type = KeyConstraintType::UNIQUE;
                    constraint.position = Peek().position;
                    Advance();
                    constraint.columns.push_back(col.name);
                    s.key_constraints.push_back(std::move(constraint));
                }
            }
            if (IsSymbol(Peek(), ",")) { Advance(); continue; }
            break;
        }
        ExpectSymbol(")");
        return s;
    }
    CreateIndexStatement ParseCreateIndex() {
        Advance();
        CreateIndexStatement s;
        s.index_name = ExpectIdentifier("index name").text;
        ExpectKeyword("ON");
        s.table = ExpectIdentifier("table name").text;
        ExpectSymbol("(");
        s.column = ExpectIdentifier("indexed column name").text;
        if (IsSymbol(Peek(), ","))
            Fail("composite indexes are not supported", Peek().position);
        ExpectSymbol(")");
        return s;
    }

    InsertStatement ParseInsert() {
        Advance(); ExpectKeyword("INTO");
        InsertStatement s;
        s.table = ExpectIdentifier("table name").text;
        ExpectKeyword("VALUES"); ExpectSymbol("(");
        while (true) {
            s.values.push_back(ParseValue());
            if (IsSymbol(Peek(), ",")) { Advance(); continue; }
            break;
        }
        ExpectSymbol(")");
        return s;
    }
    static bool IsAggregateFunction(const Token& token) {
        return IsKeyword(token, "COUNT") || IsKeyword(token, "SUM") ||
               IsKeyword(token, "AVG") || IsKeyword(token, "MIN") ||
               IsKeyword(token, "MAX");
    }

    AggregateExpression ParseAggregateExpression() {
        const Token function = Advance();
        AggregateExpression expression;
        expression.position = function.position;
        if (function.text == "COUNT") expression.function = AggregateFunction::COUNT;
        else if (function.text == "SUM") expression.function = AggregateFunction::SUM;
        else if (function.text == "AVG") expression.function = AggregateFunction::AVG;
        else if (function.text == "MIN") expression.function = AggregateFunction::MIN;
        else expression.function = AggregateFunction::MAX;
        ExpectSymbol("(");
        if (IsSymbol(Peek(), "*")) {
            Advance();
            expression.star = true;
            if (expression.function != AggregateFunction::COUNT)
                Fail("only COUNT accepts '*'", function.position);
        } else {
            expression.column = ParseColumnReference();
        }
        ExpectSymbol(")");
        return expression;
    }

    SelectStatement ParseSelect() {
        Advance();
        SelectStatement s;
        if (IsKeyword(Peek(), "DISTINCT")) {
            Advance();
            s.distinct = true;
        }
        if (IsSymbol(Peek(), "*")) { Advance(); s.select_all = true; }
        else {
            while (true) {
                SelectItem item;
                if (IsAggregateFunction(Peek())) {
                    item.aggregate = ParseAggregateExpression();
                } else {
                    item.column = ParseColumnReference();
                    s.columns.push_back(item.column);
                }
                s.select_items.push_back(std::move(item));
                if (IsSymbol(Peek(), ",")) { Advance(); continue; }
                break;
            }
        }
        ExpectKeyword("FROM");
        s.table = ExpectIdentifier("table name").text;
        s.table_alias = ParseOptionalTableAlias();
        while (IsJoinStart(Peek())) {
            s.joins.push_back(ParseJoinClause());
        }
        s.where = ParseOptionalWhere();
        if (IsKeyword(Peek(), "GROUP")) {
            Advance();
            ExpectKeyword("BY");
            while (true) {
                s.group_by.push_back(ParseColumnReference());
                if (!IsSymbol(Peek(), ",")) break;
                Advance();
            }
        }
        s.having = ParseOptionalHaving();
        if (IsKeyword(Peek(), "ORDER")) {
            Advance();
            ExpectKeyword("BY");
            while (true) {
                OrderByItem item;
                item.column = ParseColumnReference(&item.position);
                if (IsKeyword(Peek(), "ASC")) Advance();
                else if (IsKeyword(Peek(), "DESC")) {
                    Advance();
                    item.descending = true;
                }
                s.order_by.push_back(std::move(item));
                if (!IsSymbol(Peek(), ",")) break;
                Advance();
            }
        }
        if (IsKeyword(Peek(), "LIMIT")) {
            Advance();
            s.limit = ParseNonNegativeInteger("LIMIT");
            if (IsKeyword(Peek(), "OFFSET")) {
                Advance();
                s.offset = ParseNonNegativeInteger("OFFSET");
            }
        } else if (IsKeyword(Peek(), "OFFSET")) {
            Advance();
            s.offset = ParseNonNegativeInteger("OFFSET");
        }
        return s;
    }

    std::size_t ParseNonNegativeInteger(const char* clause) {
        if (Peek().type != TokenType::INTEGER)
            Fail(std::string(clause) + " requires a non-negative integer", Peek().position);
        const Token& token = Advance();
        try {
            const long long value = std::stoll(token.text);
            if (value < 0) Fail(std::string(clause) + " requires a non-negative integer", token.position);
            return static_cast<std::size_t>(value);
        } catch (const std::out_of_range&) {
            Fail(std::string(clause) + " value is out of range", token.position);
        }
    }

    static bool IsJoinStart(const Token& token) {
        return IsKeyword(token, "JOIN") || IsKeyword(token, "INNER") ||
               IsKeyword(token, "LEFT") || IsKeyword(token, "RIGHT") ||
               IsKeyword(token, "FULL") || IsKeyword(token, "CROSS");
    }

    JoinClause ParseJoinClause() {
        JoinClause join;
        join.position = Peek().position;

        if (IsKeyword(Peek(), "INNER")) {
            Advance();
            join.type = JoinType::INNER;
            ExpectKeyword("JOIN");
        } else if (IsKeyword(Peek(), "LEFT")) {
            Advance();
            join.type = JoinType::LEFT;
            if (IsKeyword(Peek(), "OUTER")) Advance();
            ExpectKeyword("JOIN");
        } else if (IsKeyword(Peek(), "RIGHT")) {
            Advance();
            join.type = JoinType::RIGHT;
            if (IsKeyword(Peek(), "OUTER")) Advance();
            ExpectKeyword("JOIN");
        } else if (IsKeyword(Peek(), "FULL")) {
            Advance();
            join.type = JoinType::FULL_OUTER;
            if (IsKeyword(Peek(), "OUTER")) Advance();
            ExpectKeyword("JOIN");
        } else if (IsKeyword(Peek(), "CROSS")) {
            Advance();
            join.type = JoinType::CROSS;
            ExpectKeyword("JOIN");
        } else {
            join.type = JoinType::INNER;
            ExpectKeyword("JOIN");
        }

        join.table = ExpectIdentifier("joined table name").text;
        join.alias = ParseOptionalTableAlias();
        if (join.type == JoinType::CROSS) {
            if (IsKeyword(Peek(), "ON"))
                Fail("CROSS JOIN does not accept an ON clause", Peek().position);
            return join;
        }

        ExpectKeyword("ON");
        std::size_t left_position = 0;
        const std::string left = ParseColumnReference(&left_position);
        if (Peek().type != TokenType::OPERATOR || Peek().text != "=")
            Fail("JOIN ON currently supports column equality only", Peek().position);
        Advance();
        const std::string right = ParseColumnReference();
        join.on = JoinCondition{left, right, left_position};
        return join;
    }

    UpdateStatement ParseUpdate() {
        Advance();
        UpdateStatement s;
        s.table = ExpectIdentifier("table name").text;
        ExpectKeyword("SET");
        while (true) {
            Assignment a;
            const Token& col = ExpectIdentifier("column name");
            a.column = col.text; a.position = col.position;
            if (Peek().type != TokenType::OPERATOR || Peek().text != "=")
                Fail("expected '=' but found " + Describe(Peek()), Peek().position);
            Advance();
            a.value = ParseValue();
            s.assignments.push_back(a);
            if (IsSymbol(Peek(), ",")) { Advance(); continue; }
            break;
        }
        s.where = ParseOptionalWhere();
        return s;
    }
    DeleteStatement ParseDelete() {
        Advance(); ExpectKeyword("FROM");
        DeleteStatement s;
        s.table = ExpectIdentifier("table name").text;
        s.where = ParseOptionalWhere();
        return s;
    }
    std::optional<Condition> ParseOptionalWhere() {
        if (!IsKeyword(Peek(), "WHERE")) return std::nullopt;
        Advance();
        Condition condition;
        pending_comparisons_.clear();
        condition.predicates.push_back(ParsePredicate());
        while (IsKeyword(Peek(), "AND") || IsKeyword(Peek(), "OR")) {
            condition.connectors.push_back(
                Peek().text == "AND" ? LogicalOp::AND : LogicalOp::OR);
            Advance();
            condition.predicates.push_back(ParsePredicate());
        }
        condition.comparisons = pending_comparisons_;
        return condition;
    }

    std::optional<Condition> ParseOptionalHaving() {
        if (!IsKeyword(Peek(), "HAVING")) return std::nullopt;
        Advance();
        Condition condition;
        pending_comparisons_.clear();
        condition.predicates.push_back(ParsePredicate());
        while (IsKeyword(Peek(), "AND") || IsKeyword(Peek(), "OR")) {
            condition.connectors.push_back(
                Peek().text == "AND" ? LogicalOp::AND : LogicalOp::OR);
            Advance();
            condition.predicates.push_back(ParsePredicate());
        }
        condition.comparisons = pending_comparisons_;
        return condition;
    }

    ConditionPredicate ParsePredicate() {
        bool negated = false;
        if (IsKeyword(Peek(), "NOT")) {
            negated = true;
            Advance();
        }

        ConditionPredicate predicate;
        predicate.negated = negated;
        if (IsAggregateFunction(Peek())) {
            predicate.aggregate = ParseAggregateExpression();
            predicate.position = predicate.aggregate->position;
            if (negated) Fail("NOT is not supported before an aggregate predicate", predicate.position);
            const Token& aggregate_op = Peek();
            if (aggregate_op.type != TokenType::OPERATOR)
                Fail("aggregate predicate requires a comparison operator", aggregate_op.position);
            predicate.type = PredicateType::COMPARISON;
            if (aggregate_op.text == "=") predicate.op = CompareOp::EQ;
            else if (aggregate_op.text == "!=" || aggregate_op.text == "<>") predicate.op = CompareOp::NE;
            else if (aggregate_op.text == "<") predicate.op = CompareOp::LT;
            else if (aggregate_op.text == ">") predicate.op = CompareOp::GT;
            else if (aggregate_op.text == "<=") predicate.op = CompareOp::LE;
            else if (aggregate_op.text == ">=") predicate.op = CompareOp::GE;
            else Fail("invalid aggregate comparison operator", aggregate_op.position);
            Advance();
            predicate.values.push_back(ParseValue());
            return predicate;
        }
        predicate.column = ParseColumnReference(&predicate.position);

        // SQL also permits `column NOT LIKE/IN/BETWEEN ...`.
        bool infix_not = false;
        if (IsKeyword(Peek(), "NOT")) {
            infix_not = true;
            Advance();
        }
        if (negated && infix_not)
            Fail("duplicate NOT in condition", Peek().position);

        if (IsKeyword(Peek(), "IS")) {
            if (infix_not) Fail("NOT is not valid before IS", Peek().position);
            Advance();
            predicate.type = PredicateType::IS_NULL;
            if (IsKeyword(Peek(), "NOT")) {
                predicate.negated = !predicate.negated;
                Advance();
            }
            ExpectKeyword("NULL");
            return predicate;
        }

        if (IsKeyword(Peek(), "LIKE")) {
            Advance();
            predicate.type = PredicateType::LIKE;
            predicate.negated = predicate.negated != infix_not;
            predicate.values.push_back(ParseValue());
            return predicate;
        }
        if (IsKeyword(Peek(), "IN")) {
            Advance();
            predicate.type = PredicateType::IN;
            predicate.negated = predicate.negated != infix_not;
            ExpectSymbol("(");
            predicate.values.push_back(ParseValue());
            while (IsSymbol(Peek(), ",")) {
                Advance();
                predicate.values.push_back(ParseValue());
            }
            ExpectSymbol(")");
            return predicate;
        }
        if (IsKeyword(Peek(), "BETWEEN")) {
            Advance();
            predicate.type = PredicateType::BETWEEN;
            predicate.negated = predicate.negated != infix_not;
            predicate.values.push_back(ParseValue());
            ExpectKeyword("AND");
            predicate.values.push_back(ParseValue());
            return predicate;
        }
        if (infix_not)
            Fail("expected LIKE, IN or BETWEEN after NOT", Peek().position);

        const Token& op = Peek();
        if (op.type != TokenType::OPERATOR)
            Fail("expected comparison operator, IS, LIKE, IN or BETWEEN but found " +
                 Describe(op), op.position);
        predicate.type = PredicateType::COMPARISON;
        if (op.text == "=") predicate.op = CompareOp::EQ;
        else if (op.text == "!=" || op.text == "<>") predicate.op = CompareOp::NE;
        else if (op.text == "<") predicate.op = CompareOp::LT;
        else if (op.text == ">") predicate.op = CompareOp::GT;
        else if (op.text == "<=") predicate.op = CompareOp::LE;
        else if (op.text == ">=") predicate.op = CompareOp::GE;
        else Fail("invalid comparison operator '" + op.text + "'", op.position);
        Advance();
        predicate.values.push_back(ParseValue());

        Comparison comparison;
        comparison.column = predicate.column;
        comparison.op = predicate.op;
        comparison.value = predicate.values.front();
        comparison.position = predicate.position;
        // Kept for existing clients that inspect the legacy comparison list.
        pending_comparisons_.push_back(comparison);
        return predicate;
    }
    Value ParseValue() {
        const Token& t = Peek();
        Value v; v.position = t.position;
        if (t.type == TokenType::INTEGER) {
            v.type = Value::Type::INTEGER;
            try { v.int_value = std::stoll(t.text); }
            catch (const std::out_of_range&) {
                Fail("integer " + t.text + " is out of range", t.position);
            }
        } else if (t.type == TokenType::STRING) {
            v.type = Value::Type::STRING; v.string_value = t.text;
        } else {
            Fail("expected an integer or string value but found " + Describe(t), t.position);
        }
        Advance();
        return v;
    }

    std::vector<Token> tokens_;
    std::vector<Comparison> pending_comparisons_;
    std::size_t index_ = 0;
};
}  // namespace

Statement Parse(const std::string& sql) {
    Parser parser(Tokenize(sql));
    return parser.ParseOne();
}
}  // namespace sql
}  // namespace ocera
