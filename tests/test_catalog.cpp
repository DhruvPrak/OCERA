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

static CreateIndexStatement MakeIndex(const std::string& name,
                                      const std::string& table,
                                      const std::string& column) {
    CreateIndexStatement s;
    s.index_name = name;
    s.table = table;
    s.column = column;
    return s;
}

static bool ThrowsIndex(Catalog& c, const CreateIndexStatement& s) {
    try { c.CreateIndex(s); return false; } catch (const CatalogError&) { return true; }
}

static Statement CreateIndexStmt(const std::string& name,
                                 const std::string& table,
                                 const std::string& column) {
    return MakeIndex(name, table, column);
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


    std::cout << "[10] CREATE INDEX validation\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(Valid(CreateIndexStmt("idx_students_id", "students", "id"), c),
              "valid INT index");
        Check(Valid(CreateIndexStmt("IDX_STUDENTS_ID", "STUDENTS", "ID"), c),
              "validation is case-insensitive before registration");
        Check(!Valid(CreateIndexStmt("", "students", "id"), c), "empty index name");
        Check(!Valid(CreateIndexStmt("idx_missing", "missing", "id"), c),
              "missing table");
        Check(!Valid(CreateIndexStmt("idx_age", "students", "age"), c),
              "missing column");
        Check(!Valid(CreateIndexStmt("idx_name", "students", "name"), c),
              "TEXT column rejected");
        c.CreateIndex(MakeIndex("idx_id", "students", "id"));
        Check(!Valid(CreateIndexStmt("idx_id", "students", "id"), c),
              "duplicate index name");
        Check(!Valid(CreateIndexStmt("idx_id_again", "STUDENTS", "ID"), c),
              "second index on same table column");
        Check(c.NumIndexes() == 1, "validation does not register indexes");
    }
    Report("CREATE INDEX validation", before);

    std::cout << "[11] CREATE INDEX catalog registration and lookups\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(c.NumIndexes() == 0 && !c.HasIndex("idx_students_id"),
              "starts without indexes");
        c.CreateIndex(MakeIndex("Idx_Students_Id", "STUDENTS", "ID"));
        Check(c.NumIndexes() == 1, "one index registered");
        Check(c.HasIndex("idx_students_id"), "index name lookup is case-insensitive");
        const IndexSchema* by_name = c.GetIndex("IDX_STUDENTS_ID");
        Check(by_name != nullptr, "GetIndex finds index case-insensitively");
        if (by_name) {
            Check(by_name->name == "Idx_Students_Id", "original index spelling kept");
            Check(by_name->table_name == "students", "canonical table spelling stored");
            Check(by_name->column_name == "id", "canonical column spelling stored");
        }
        const IndexSchema* by_column = c.GetIndexForColumn("Students", "ID");
        Check(by_column != nullptr, "lookup by table and column is case-insensitive");
        Check(by_column && by_column->name == "Idx_Students_Id",
              "column lookup returns registered index");
        Check(c.GetIndex("missing") == nullptr, "missing index returns nullptr");
        Check(c.GetIndexForColumn("students", "name") == nullptr,
              "unindexed column returns nullptr");
    }
    Report("CREATE INDEX registration", before);

    std::cout << "[12] Rejected CREATE INDEX operations leave Catalog unchanged\n";
    before = g_failures;
    {
        Catalog c = MakeStudents();
        Check(ThrowsIndex(c, MakeIndex("", "students", "id")), "empty index name");
        Check(ThrowsIndex(c, MakeIndex("idx_missing", "missing", "id")),
              "missing table");
        Check(ThrowsIndex(c, MakeIndex("idx_age", "students", "age")),
              "missing column");
        Check(ThrowsIndex(c, MakeIndex("idx_name", "students", "name")),
              "TEXT column");
        Check(!c.HasIndex("idx_missing") && !c.HasIndex("idx_age") &&
              !c.HasIndex("idx_name"), "failed registrations leave no metadata");
        Check(c.NumIndexes() == 0, "failed registrations preserve index count");

        c.CreateIndex(MakeIndex("idx_id", "students", "id"));
        Check(ThrowsIndex(c, MakeIndex("IDX_ID", "students", "id")),
              "duplicate index name, case-insensitive");
        Check(ThrowsIndex(c, MakeIndex("idx_id_second", "STUDENTS", "ID")),
              "duplicate index for table/column, case-insensitive");
        Check(c.NumIndexes() == 1 && c.HasIndex("idx_id") &&
              !c.HasIndex("idx_id_second"), "duplicate failures leave existing index intact");
    }
    Report("CREATE INDEX rejection / atomicity", before);

    std::cout << "[13] PRIMARY KEY / UNIQUE metadata validation" << std::endl;
    before = g_failures;
    {
        Catalog c;
        CreateTableStatement keyed = MakeCreate("enrollments",
            {{"student_id", ColumnType::INT}, {"course_id", ColumnType::INT},
             {"email", ColumnType::TEXT}});
        keyed.key_constraints.push_back(
            {KeyConstraintType::PRIMARY_KEY, {"student_id", "course_id"}, 0});
        keyed.key_constraints.push_back(
            {KeyConstraintType::UNIQUE, {"email"}, 0});
        c.CreateTable(keyed);
        const TableSchema* schema = c.GetTable("enrollments");
        Check(schema && schema->key_constraints.size() == 2,
              "catalog retains primary and unique key metadata");
        Check(schema && schema->key_constraints[0].columns.size() == 2,
              "composite primary key metadata retained");

        CreateTableStatement unknown = MakeCreate("bad_key", {{"id", ColumnType::INT}});
        unknown.key_constraints.push_back(
            {KeyConstraintType::PRIMARY_KEY, {"missing"}, 0});
        Check(ThrowsCatalog(c, unknown), "unknown key column rejected");

        CreateTableStatement two_pk = MakeCreate("bad_pk",
            {{"a", ColumnType::INT}, {"b", ColumnType::INT}});
        two_pk.key_constraints.push_back({KeyConstraintType::PRIMARY_KEY, {"a"}, 0});
        two_pk.key_constraints.push_back({KeyConstraintType::PRIMARY_KEY, {"b"}, 0});
        Check(ThrowsCatalog(c, two_pk), "multiple primary keys rejected");

        CreateTableStatement duplicate_col = MakeCreate("bad_duplicate_key",
            {{"a", ColumnType::INT}, {"b", ColumnType::INT}});
        duplicate_col.key_constraints.push_back(
            {KeyConstraintType::UNIQUE, {"a", "A"}, 0});
        Check(ThrowsCatalog(c, duplicate_col), "duplicate key column rejected");
    }
    Report("key metadata / validation", before);

    std::cout << "[14] FOREIGN KEY Catalog metadata and validation" << std::endl;
    before = g_failures;
    {
        Catalog c;
        CreateTableStatement parent = MakeCreate(
            "Students", {{"student_id", ColumnType::INT}, {"email", ColumnType::TEXT}});
        parent.key_constraints.push_back(
            {KeyConstraintType::PRIMARY_KEY, {"student_id"}, 0});
        parent.key_constraints.push_back(
            {KeyConstraintType::UNIQUE, {"email"}, 0});
        c.CreateTable(parent);

        CreateTableStatement child = MakeCreate(
            "Enrollments", {{"student_id", ColumnType::INT}, {"course_id", ColumnType::INT}});
        child.key_constraints.push_back(
            {KeyConstraintType::PRIMARY_KEY, {"student_id", "course_id"}, 0});
        child.foreign_key_constraints.push_back(
            {{"student_id"}, "students", {"STUDENT_ID"}, 0});
        c.CreateTable(child);
        const TableSchema* child_schema = c.GetTable("enrollments");
        Check(child_schema && child_schema->foreign_key_constraints.size() == 1,
              "single-column FK metadata stored");
        if (child_schema && !child_schema->foreign_key_constraints.empty()) {
            const auto& fk = child_schema->foreign_key_constraints[0];
            Check(fk.columns[0] == "student_id" &&
                  fk.referenced_table == "Students" &&
                  fk.referenced_columns[0] == "student_id",
                  "FK identifiers canonicalized to catalog spelling");
        }

        CreateTableStatement composite_parent = MakeCreate(
            "StudentCourses", {{"student_id", ColumnType::INT}, {"course_id", ColumnType::INT}});
        composite_parent.key_constraints.push_back(
            {KeyConstraintType::PRIMARY_KEY, {"student_id", "course_id"}, 0});
        c.CreateTable(composite_parent);

        CreateTableStatement composite_child = MakeCreate(
            "Grades", {{"student_id", ColumnType::INT}, {"course_id", ColumnType::INT},
                       {"grade", ColumnType::INT}});
        composite_child.foreign_key_constraints.push_back(
            {{"student_id", "course_id"}, "StudentCourses",
             {"student_id", "course_id"}, 0});
        Check(Valid(composite_child, c), "composite FK passes semantic validation");
        c.CreateTable(composite_child);
        const TableSchema* grades = c.GetTable("grades");
        Check(grades && grades->foreign_key_constraints.size() == 1 &&
              grades->foreign_key_constraints[0].columns.size() == 2,
              "composite FK metadata stored");

        CreateTableStatement unique_child = MakeCreate(
            "EmailAliases", {{"email", ColumnType::TEXT}});
        unique_child.foreign_key_constraints.push_back(
            {{"email"}, "Students", {"email"}, 0});
        Check(Valid(unique_child, c), "FK may reference a UNIQUE key");

        CreateTableStatement missing_table = MakeCreate(
            "BadMissingTable", {{"student_id", ColumnType::INT}});
        missing_table.foreign_key_constraints.push_back(
            {{"student_id"}, "NoSuchParent", {"id"}, 0});
        Check(!Valid(missing_table, c), "validator rejects missing referenced table");
        Check(ThrowsCatalog(c, missing_table), "missing referenced table rejected");

        CreateTableStatement missing_parent_column = MakeCreate(
            "BadMissingColumn", {{"student_id", ColumnType::INT}});
        missing_parent_column.foreign_key_constraints.push_back(
            {{"student_id"}, "Students", {"missing"}, 0});
        Check(ThrowsCatalog(c, missing_parent_column), "missing referenced column rejected");

        CreateTableStatement missing_local_column = MakeCreate(
            "BadLocalColumn", {{"student_id", ColumnType::INT}});
        missing_local_column.foreign_key_constraints.push_back(
            {{"missing"}, "Students", {"student_id"}, 0});
        Check(ThrowsCatalog(c, missing_local_column), "missing local column rejected");

        CreateTableStatement not_unique = MakeCreate(
            "BadNotUnique", {{"name", ColumnType::TEXT}});
        not_unique.foreign_key_constraints.push_back(
            {{"name"}, "Students", {"name"}, 0});
        Check(!Valid(not_unique, c), "validator rejects non-unique referenced columns");
        Check(ThrowsCatalog(c, not_unique), "non-unique referenced columns rejected");

        CreateTableStatement wrong_types = MakeCreate(
            "BadTypes", {{"student_id", ColumnType::TEXT}});
        wrong_types.foreign_key_constraints.push_back(
            {{"student_id"}, "Students", {"student_id"}, 0});
        Check(!Valid(wrong_types, c), "validator rejects mismatched types");
        Check(ThrowsCatalog(c, wrong_types), "mismatched types rejected");

        CreateTableStatement wrong_arity = MakeCreate(
            "BadArity", {{"student_id", ColumnType::INT}});
        wrong_arity.foreign_key_constraints.push_back(
            {{"student_id"}, "StudentCourses", {"student_id", "course_id"}, 0});
        Check(ThrowsCatalog(c, wrong_arity), "mismatched column counts rejected");

        CreateTableStatement duplicate_columns = MakeCreate(
            "BadDuplicateFK", {{"student_id", ColumnType::INT}, {"course_id", ColumnType::INT}});
        duplicate_columns.foreign_key_constraints.push_back(
            {{"student_id", "STUDENT_ID"}, "StudentCourses",
             {"student_id", "course_id"}, 0});
        Check(ThrowsCatalog(c, duplicate_columns), "duplicate local FK columns rejected");

        CreateTableStatement wrong_order = MakeCreate(
            "BadOrder", {{"student_id", ColumnType::INT}, {"course_id", ColumnType::INT}});
        wrong_order.foreign_key_constraints.push_back(
            {{"student_id", "course_id"}, "StudentCourses",
             {"course_id", "student_id"}, 0});
        Check(ThrowsCatalog(c, wrong_order),
              "referenced tuple must match candidate-key column order");

        CreateTableStatement self_reference = MakeCreate(
            "Employees", {{"id", ColumnType::INT}, {"manager_id", ColumnType::INT}});
        self_reference.key_constraints.push_back(
            {KeyConstraintType::PRIMARY_KEY, {"id"}, 0});
        self_reference.foreign_key_constraints.push_back(
            {{"manager_id"}, "Employees", {"id"}, 0});
        Check(!ThrowsCatalog(c, self_reference), "self-referencing FK accepted");

        Check(!c.HasTable("BadMissingTable") && !c.HasTable("BadMissingColumn") &&
              !c.HasTable("BadLocalColumn") && !c.HasTable("BadNotUnique") &&
              !c.HasTable("BadTypes") && !c.HasTable("BadArity") &&
              !c.HasTable("BadDuplicateFK") && !c.HasTable("BadOrder"),
              "failed FK registrations publish no partial table metadata");
    }
    Report("FOREIGN KEY metadata / validation", before);

    if (g_failures == 0) {
        std::cout << "\nAll catalog tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " catalog check(s) FAILED.\n";
    return 1;
}
