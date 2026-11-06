#include "sql/parser.h"

#include <utility>
#include <vector>

#include "sql/tokenizer.h"

namespace ocera {
namespace sql {

namespace {

// A small "recursive descent" parser: one function per grammar rule, each
// consuming tokens from left to right. Peek() looks at the next token
// without consuming it; Advance() consumes it. The token list always ends
// with END_OF_INPUT and Advance() never moves past it, so we cannot read
// beyond the end of the vector.
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    Statement ParseOne() {
        Statement stmt = ParseBody();
        if (IsSymbol(Peek(), ";")) Advance();
        if (Peek().type != TokenType::END_OF_INPUT) {
            Fail("unexpected " + Describe(Peek()) + " after end of statement",
                 Peek().position);
        }
        return stmt;
    }

private:
    // ---- token helpers ----------------------------------------------------
    const Token& Peek() const { return tokens_[index_]; }

    const Token& Advance() {
        const Token& t = tokens_[index_];
        if (t.type != TokenType::END_OF_INPUT) ++index_;
        return t;
    }

    [[noreturn]] static void Fail(const std::string& message, std::size_t pos) {
        throw ParseError(message, pos);
    }

    static std::string Describe(const Token& t) {
        switch (t.type) {
            case TokenType::END_OF_INPUT: return "end of input";
            case TokenType::STRING: return "string '" + t.text + "'";
            default: return "'" + t.text + "'";
        }
    }

    static bool IsKeyword(const Token& t, const char* kw) {
        return t.type == TokenType::KEYWORD && t.text == kw;
    }
    static bool IsSymbol(const Token& t, const char* sym) {
        return t.type == TokenType::SYMBOL && t.text == sym;
    }

    const Token& ExpectKeyword(const char* kw) {
        if (!IsKeyword(Peek(), kw)) {
            Fail(std::string("expected ") + kw + " but found " + Describe(Peek()),
                 Peek().position);
        }
        return Advance();
    }

    const Token& ExpectSymbol(const char* sym) {
        if (!IsSymbol(Peek(), sym)) {
            Fail(std::string("expected '") + sym + "' but found " + Describe(Peek()),
                 Peek().position);
        }
        return Advance();
    }

    const Token& ExpectIdentifier(const char* what) {
        if (Peek().type != TokenType::IDENTIFIER) {
            Fail(std::string("expected ") + what + " but found " + Describe(Peek()),
                 Peek().position);
        }
        return Advance();
    }

    // ---- grammar rules ----------------------------------------------------
    Statement ParseBody() {
        const Token& first = Peek();
        if (first.type == TokenType::END_OF_INPUT) {
            Fail("empty statement", first.position);
        }
        if (IsKeyword(first, "CREATE")) return ParseCreate();
        if (IsKeyword(first, "INSERT")) return ParseInsert();
        if (IsKeyword(first, "SELECT")) return ParseSelect();
        if (IsKeyword(first, "UPDATE")) return ParseUpdate();
        if (IsKeyword(first, "DELETE")) return ParseDelete();
        Fail("expected CREATE, INSERT, SELECT, UPDATE or DELETE but found " +
                 Describe(first),
             first.position);
    }

    // CREATE TABLE name ( col type [, col type]... )
    CreateTableStatement ParseCreate() {
        Advance();  // CREATE
        ExpectKeyword("TABLE");
        CreateTableStatement s;
        s.table = ExpectIdentifier("table name").text;
        ExpectSymbol("(");
        while (true) {
            ColumnDefinition col;
            const Token& name = ExpectIdentifier("column name");
            col.name = name.text;
            col.position = name.position;
            const Token& type = Peek();
            if (IsKeyword(type, "INT")) {
                col.type = ColumnType::INT;
            } else if (IsKeyword(type, "TEXT")) {
                col.type = ColumnType::TEXT;
            } else {
                Fail("expected column type INT or TEXT but found " + Describe(type),
                     type.position);
            }
            Advance();
            s.columns.push_back(col);
            if (IsSymbol(Peek(), ",")) {
                Advance();
                continue;
            }
            break;
        }
        ExpectSymbol(")");
        return s;
    }

    // INSERT INTO name VALUES ( value [, value]... )
    InsertStatement ParseInsert() {
        Advance();  // INSERT
        ExpectKeyword("INTO");
        InsertStatement s;
        s.table = ExpectIdentifier("table name").text;
        ExpectKeyword("VALUES");
        ExpectSymbol("(");
        while (true) {
            s.values.push_back(ParseValue());
            if (IsSymbol(Peek(), ",")) {
                Advance();
                continue;
            }
            break;
        }
        ExpectSymbol(")");
        return s;
    }

    // SELECT ( * | col [, col]... ) FROM name [WHERE cond]
    SelectStatement ParseSelect() {
        Advance();  // SELECT
        SelectStatement s;
        if (IsSymbol(Peek(), "*")) {
            Advance();
            s.select_all = true;
        } else {
            while (true) {
                s.columns.push_back(ExpectIdentifier("column name or '*'").text);
                if (IsSymbol(Peek(), ",")) {
                    Advance();
                    continue;
                }
                break;
            }
        }
        ExpectKeyword("FROM");
        s.table = ExpectIdentifier("table name").text;
        s.where = ParseOptionalWhere();
        return s;
    }

    // UPDATE name SET col = value [, col = value]... [WHERE cond]
    UpdateStatement ParseUpdate() {
        Advance();  // UPDATE
        UpdateStatement s;
        s.table = ExpectIdentifier("table name").text;
        ExpectKeyword("SET");
        while (true) {
            Assignment a;
            const Token& col = ExpectIdentifier("column name");
            a.column = col.text;
            a.position = col.position;
            if (Peek().type != TokenType::OPERATOR || Peek().text != "=") {
                Fail("expected '=' but found " + Describe(Peek()), Peek().position);
            }
            Advance();
            a.value = ParseValue();
            s.assignments.push_back(a);
            if (IsSymbol(Peek(), ",")) {
                Advance();
                continue;
            }
            break;
        }
        s.where = ParseOptionalWhere();
        return s;
    }

    // DELETE FROM name [WHERE cond]
    DeleteStatement ParseDelete() {
        Advance();  // DELETE
        ExpectKeyword("FROM");
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
            c.connectors.push_back(Peek().text == "AND" ? LogicalOp::AND
                                                         : LogicalOp::OR);
            Advance();
            c.comparisons.push_back(ParseComparison());
        }
        return c;
    }

    // col op value
    Comparison ParseComparison() {
        Comparison cmp;
        const Token& col = ExpectIdentifier("column name");
        cmp.column = col.text;
        cmp.position = col.position;
        const Token& op = Peek();
        if (op.type != TokenType::OPERATOR) {
            Fail("expected comparison operator (=, !=, <, >, <=, >=) but found " +
                     Describe(op),
                 op.position);
        }
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

    // integer | 'string'
    Value ParseValue() {
        const Token& t = Peek();
        Value v;
        v.position = t.position;
        if (t.type == TokenType::INTEGER) {
            v.type = Value::Type::INTEGER;
            try {
                v.int_value = std::stoll(t.text);
            } catch (const std::out_of_range&) {
                Fail("integer " + t.text + " is out of range", t.position);
            }
        } else if (t.type == TokenType::STRING) {
            v.type = Value::Type::STRING;
            v.string_value = t.text;
        } else {
            Fail("expected an integer or string value but found " + Describe(t),
                 t.position);
        }
        Advance();
        return v;
    }

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
};

}  // namespace

Statement Parse(const std::string& sql) {
    Parser parser(Tokenize(sql));  // lexical errors propagate as TokenizerError
    return parser.ParseOne();
}

}  // namespace sql
}  // namespace ocera
