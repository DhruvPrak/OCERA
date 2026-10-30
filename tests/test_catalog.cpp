#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/validator.h"

using namespace ocera;
using namespace ocera::sql;

static int g_failures = 0;
static void Check(bool cond, const std::string& what) {
    if (!cond) {
        std::cout << "    FAILED: " << what << "\n";
        ++g_failures;
    }
}
static void Report(const char* name, int before) {
    std::cout << (g_failures == before ? "    PASSED: " : "    (see failures above) ") << name << "\n";
}

// ---- helpers that build AST nodes directly (no parser needed) ----
static Value Int(std::int64_t v) { Value x; x.type = Value::Type::INTEGER; x.int_value = v; return x; }
static Value Str(const std::string& s) { Value x; x.type = Value::Type::STRING; x.string_value = s; return x; }
static Comparison Cmp(const std::string& col, CompareOp op, const Value& v) {
    Comparison c; c.column = col; c.op = op; c.value = v; return c;
}
static Condition Where(std::vector<Comparison> cmps, std::vector<LogicalOp> conns = {}) {
    Condition c; c.comparisons = std::move(cmps); c.connectors = std::move(conns); return c;
}
static CreateTableStatement MakeCreate(const std::string& name,
                                       const std::vector<std::pair<std::string, ColumnType>>& cols) {
    CreateTableStatement s; s.table = name;
    for (auto& c : cols) { ColumnDefinition d; d.name = c.first; d.type = c.second; s.columns.push_back(d); }
    return s;
}
static Statement Insert(const std::string& t, std::vector<Value> vals) {
    InsertStatement s; s.table = t; s.values = std::move(vals); return s;
}
static Statement Select(const std::string& t, std::vector<std::string> cols,
                        std::optional<Condition> w = std::nullopt) {
    SelectStatement s; s.table = t; s.select_all = cols.empty(); s.columns = std::move(cols); s.where = std::move(w); return s;
}
static Statement Update(const std::string& t, std::vector<std::pair<std::string, Value>> sets,
                        std::optional<Condition> w = std::nullopt) {
    UpdateStatement s; s.table = t;
    for (auto& a : sets) { Assignment x; x.column = a.first; x.value = a.second; s.assignments.push_back(x); }
    s.where = std::move(w); return s;
}
static Statement Delete(const std::string& t, std::optional<Condition> w = std::nullopt) {
    DeleteStatement s; s.table = t; s.where = std::move(w); return s;
}

static bool Valid(const Statement& st, const Catalog& c) {
    try { Validate(st, c); return true; } catch (const SemanticError&) { return false; }
}
static bool ThrowsCatalog(Catalog& c, const CreateTableStatement& s) {
    try { c.CreateTable(s); return false; } catch (const CatalogError&) { return true; }
}

// students(id INT, name TEXT)
static Catalog MakeStudents() {
    Catalog c;
    c.CreateTable(MakeCreate("students", {{"id", ColumnType::INT}, {"name", ColumnType::TEXT}}));
    return c;
}

int main() {
    std::cout << "=== OCERA Catalog + Validator Test (in-memory) ===\n\n";
    int before;

    std::cout << "[1] Create a table and look it up\n";
    before = g_failures;
    {
        Catalog c;
        Check(c.NumTables() == 0 && !c.HasTable("students"), "starts empty");
        c.CreateTable(MakeCreate("Students", {{"Id", ColumnType::INT}, {"name", ColumnType::TEXT}}));
        Check(c.NumTables() == 1, "one table");
        const TableSchema* t = c.GetTable("students");
        Check(t != nullptr, "found (case-insensitive)");
        if (t) {
            Check(t->name == "Students", "original spelling of table kept");
            Check(t->columns.size() == 2, "two columns");
            Check(t->columns[0].name == "Id" && t->columns[0].type == ColumnType::INT, "column 0");
            Check(t->columns[1].name == "name" && t->columns[1].type == ColumnType::TEXT, "column 1");
            Check(t->ColumnIndex("ID") == 0 && t->ColumnIndex("NAME") == 1, "column lookup is case-insensitive");
            Check(t->ColumnIndex("age") == -1, "unknown column gives -1");
        }
        Check(c.HasTable("STUDENTS"), "HasTable case-insensitive");
        Check(c.GetTable("teachers") == nullptr && !c.HasTable("teachers"), "missing table");
    }
    Report("create / lookup", before);

    std::cout << "[2] Rejected CREATE TABLEs\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(ThrowsCatalog(c, MakeCreate("students", {{"x", ColumnType::INT}})), "duplicate table");
        Check(ThrowsCatalog(c, MakeCreate("STUDENTS", {{"x", ColumnType::INT}})), "duplicate table, different case");
        Check(ThrowsCatalog(c, MakeCreate("t", {{"a", ColumnType::INT}, {"A", ColumnType::TEXT}})), "duplicate column names");
        Check(ThrowsCatalog(c, MakeCreate("t", {})), "table with no columns");
        Check(!c.HasTable("t"), "failed creates leave nothing behind");
        Check(c.NumTables() == 1, "catalog unchanged after failures");
    }
    Report("rejected creates", before);

    std::cout << "[3] TableNames are alphabetical\n";
    before = g_failures;
    {
        Catalog c;
        c.CreateTable(MakeCreate("zebra", {{"a", ColumnType::INT}}));
        c.CreateTable(MakeCreate("Apple", {{"a", ColumnType::INT}}));
        c.CreateTable(MakeCreate("mango", {{"a", ColumnType::INT}}));
        auto n = c.TableNames();
        Check(n.size() == 3 && n[0] == "Apple" && n[1] == "mango" && n[2] == "zebra", "sorted, original spelling");
    }
    Report("table names", before);

    std::cout << "[4] Valid statements of all five kinds\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(Valid(Insert("students", {Int(1), Str("Asha")}), c), "insert");
        Check(Valid(Select("students", {}), c), "select *");
        Check(Valid(Select("students", {"name", "ID"}), c), "select columns (case-insensitive)");
        Check(Valid(Select("students", {"name"}, Where({Cmp("id", CompareOp::EQ, Int(1))})), c), "select with where");
        Check(Valid(Update("students", {{"name", Str("Riya")}}, Where({Cmp("id", CompareOp::GE, Int(1))})), c), "update");
        Check(Valid(Update("students", {{"id", Int(2)}, {"name", Str("x")}}), c), "update two columns, no where");
        Check(Valid(Delete("students", Where({Cmp("name", CompareOp::NE, Str("x"))})), c), "delete with where");
        Check(Valid(Delete("students"), c), "delete all");
    }
    Report("valid statements", before);

    std::cout << "[5] Unknown table\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(!Valid(Insert("nope", {Int(1)}), c), "insert");
        Check(!Valid(Select("nope", {}), c), "select");
        Check(!Valid(Update("nope", {{"a", Int(1)}}), c), "update");
        Check(!Valid(Delete("nope"), c), "delete");
    }
    Report("unknown table", before);

    std::cout << "[6] INSERT: wrong number of values or wrong types\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(!Valid(Insert("students", {Int(1)}), c), "too few values");
        Check(!Valid(Insert("students", {Int(1), Str("a"), Int(3)}), c), "too many values");
        Check(!Valid(Insert("students", {}), c), "no values");
        Check(!Valid(Insert("students", {Str("1"), Str("a")}), c), "string into INT column");
        Check(!Valid(Insert("students", {Int(1), Int(2)}), c), "integer into TEXT column");
    }
    Report("insert checks", before);

    std::cout << "[7] SELECT / UPDATE / WHERE: unknown columns and wrong types\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(!Valid(Select("students", {"age"}), c), "select unknown column");
        Check(!Valid(Select("students", {"id", "age"}), c), "select: one good, one unknown column");
        Check(!Valid(Update("students", {{"age", Int(1)}}), c), "update unknown column");
        Check(!Valid(Update("students", {{"id", Str("x")}}), c), "update wrong type");
        Check(!Valid(Select("students", {}, Where({Cmp("age", CompareOp::EQ, Int(1))})), c), "where unknown column");
        Check(!Valid(Select("students", {}, Where({Cmp("id", CompareOp::EQ, Str("x"))})), c), "where wrong type");
        Check(!Valid(Delete("students", Where({Cmp("name", CompareOp::EQ, Int(1))})), c), "delete where wrong type");
        // error in the SECOND comparison of an AND/OR chain must still be found
        Check(!Valid(Select("students", {}, Where({Cmp("id", CompareOp::EQ, Int(1)), Cmp("age", CompareOp::EQ, Int(2))},
                                                  {LogicalOp::AND})), c), "bad column in 2nd comparison");
        Check(Valid(Select("students", {}, Where({Cmp("id", CompareOp::EQ, Int(1)), Cmp("name", CompareOp::EQ, Str("a")),
                                                  Cmp("id", CompareOp::LT, Int(9))},
                                                 {LogicalOp::AND, LogicalOp::OR})), c), "valid 3-comparison AND/OR chain");
        Condition bad; bad.comparisons = {Cmp("id", CompareOp::EQ, Int(1)), Cmp("id", CompareOp::EQ, Int(2))};  // missing connector
        Check(!Valid(Select("students", {}, bad), c), "malformed condition (connector count)");
        Condition empty;
        Check(!Valid(Select("students", {}, empty), c), "malformed condition (empty)");
    }
    Report("column / type checks", before);

    std::cout << "[8] CREATE TABLE validation does not change the catalog\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(!Valid(MakeCreate("students", {{"x", ColumnType::INT}}), c), "existing table rejected");
        Check(!Valid(MakeCreate("t", {{"a", ColumnType::INT}, {"a", ColumnType::TEXT}}), c), "duplicate columns rejected");
        Check(Valid(MakeCreate("teachers", {{"id", ColumnType::INT}}), c), "new table accepted");
        Check(!c.HasTable("teachers"), "validating does not create the table");
    }
    Report("create validation", before);

    std::cout << "[9] Error messages are specific\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        try { Validate(Select("students", {"age"}), c); Check(false, "should throw"); }
        catch (const SemanticError& e) {
            std::string m = e.what();
            Check(m.find("students") != std::string::npos && m.find("age") != std::string::npos,
                  "message names table and column: " + m);
        }
        try { Validate(Insert("students", {Str("x"), Str("y")}), c); Check(false, "should throw"); }
        catch (const SemanticError& e) {
            std::string m = e.what();
            Check(m.find("INT") != std::string::npos, "message names expected type: " + m);
        }
    }
    Report("messages", before);

    if (g_failures == 0) {
        std::cout << "\nAll catalog tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " catalog check(s) FAILED.\n";
    return 1;
}
