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

    Statement ParseBody() {
        const Token& first = Peek();
        if (first.type == TokenType::END_OF_INPUT) Fail("empty statement", first.position);
        if (IsKeyword(first, "CREATE")) return ParseCreate();
        if (IsKeyword(first, "INSERT")) return ParseInsert();
        if (IsKeyword(first, "SELECT")) return ParseSelect();
        if (IsKeyword(first, "UPDATE")) return ParseUpdate();
        if (IsKeyword(first, "DELETE")) return ParseDelete();
        Fail("expected CREATE, INSERT, SELECT, UPDATE or DELETE but found " +
             Describe(first), first.position);
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
    SelectStatement ParseSelect() {
        Advance();
        SelectStatement s;
        if (IsSymbol(Peek(), "*")) { Advance(); s.select_all = true; }
        else {
            while (true) {
                s.columns.push_back(ExpectIdentifier("column name or '*'").text);
                if (IsSymbol(Peek(), ",")) { Advance(); continue; }
                break;
            }
        }
        ExpectKeyword("FROM");
        s.table = ExpectIdentifier("table name").text;
        s.where = ParseOptionalWhere();
        return s;
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
        Condition c;
        c.comparisons.push_back(ParseComparison());
        while (IsKeyword(Peek(), "AND") || IsKeyword(Peek(), "OR")) {
            c.connectors.push_back(Peek().text == "AND" ? LogicalOp::AND : LogicalOp::OR);
            Advance();
            c.comparisons.push_back(ParseComparison());
        }
        return c;
    }
    Comparison ParseComparison() {
        Comparison cmp;
        const Token& col = ExpectIdentifier("column name");
        cmp.column = col.text; cmp.position = col.position;
        const Token& op = Peek();
        if (op.type != TokenType::OPERATOR)
            Fail("expected comparison operator (=, !=, <, >, <=, >=) but found " +
                 Describe(op), op.position);
        if (op.text == "=") cmp.op = CompareOp::EQ;
        else if (op.text == "!=") cmp.op = CompareOp::NE;
        else if (op.text == "<") cmp.op = CompareOp::LT;
        else if (op.text == ">") cmp.op = CompareOp::GT;
        else if (op.text == "<=") cmp.op = CompareOp::LE;
        else cmp.op = CompareOp::GE;
        Advance();
        cmp.value = ParseValue();
        return cmp;
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
    std::size_t index_ = 0;
};
}  // namespace

Statement Parse(const std::string& sql) {
    Parser parser(Tokenize(sql));
    return parser.ParseOne();
}
}  // namespace sql
}  // namespace ocera
