
#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cli/statement_reader.h"
#include "storage/database_instance.h"

namespace {

std::string Trim(const std::string& input) {
    const auto first = std::find_if_not(
        input.begin(), input.end(),
        [](unsigned char ch) { return std::isspace(ch); }
    );

    if (first == input.end()) {
        return "";
    }

    const auto last = std::find_if_not(
        input.rbegin(), input.rend(),
        [](unsigned char ch) { return std::isspace(ch); }
    ).base();

    return std::string(first, last);
}

std::string ToLower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    return value;
}

// Convert an SQL value into printable text.
std::string ValueToString(const ocera::sql::Value& value) {
    if (value.type == ocera::sql::Value::Type::INTEGER) {
        return std::to_string(value.int_value);
    }

    return value.string_value;
}

// Print a horizontal table boundary.
void PrintTableBorder(const std::vector<std::size_t>& widths) {
    std::cout << '+';

    for (const std::size_t width : widths) {
        std::cout << std::string(width + 2, '-') << '+';
    }

    std::cout << '\n';
}

// Print one table row with properly aligned columns.
void PrintTableRow(
    const std::vector<std::string>& cells,
    const std::vector<std::size_t>& widths
) {
    std::cout << '|';

    for (std::size_t i = 0; i < widths.size(); ++i) {
        const std::string cell =
            (i < cells.size()) ? cells[i] : "";

        std::cout << ' ' << cell;

        if (cell.size() < widths[i]) {
            std::cout << std::string(widths[i] - cell.size(), ' ');
        }

        std::cout << " |";
    }

    std::cout << '\n';
}

void PrintQueryResult(const ocera::QueryResult& result) {
    const std::size_t column_count = result.column_names.size();

    // Preserve the empty-result behavior.
    if (column_count == 0) {
        std::cout << "(0 rows)\n";
        return;
    }

    std::vector<std::vector<std::string>> rows;
    rows.reserve(result.rows.size());

    // Calculate the maximum required width of each column.
    std::vector<std::size_t> widths(column_count, 0);

    for (std::size_t i = 0; i < column_count; ++i) {
        widths[i] = result.column_names[i].size();
    }

    for (const auto& row : result.rows) {
        std::vector<std::string> printable_row;
        printable_row.reserve(column_count);

        for (std::size_t i = 0; i < column_count; ++i) {
            const std::string value =
                (i < row.size()) ? ValueToString(row[i]) : "";

            printable_row.push_back(value);
            widths[i] = std::max(widths[i], value.size());
        }

        rows.push_back(std::move(printable_row));
    }

    // Draw the complete table.
    PrintTableBorder(widths);
    PrintTableRow(result.column_names, widths);
    PrintTableBorder(widths);

    for (const auto& row : rows) {
        PrintTableRow(row, widths);
    }

    PrintTableBorder(widths);

    std::cout << '(' << result.rows.size() << " rows)\n";
}

// The shell uses the same DatabaseInstance persistence path as the tests.
struct RuntimeDatabase {
    ocera::DatabaseInstance instance;
    ocera::SqlExecutor& executor;

    RuntimeDatabase(const std::string& file, ocera::DatabaseOpenMode mode)
        : instance(file, 64, mode), executor(instance.GetExecutor()) {}

    void Flush() { instance.Flush(); }
};

const char* kDatabaseRegistry = ".ocera_databases";

std::vector<std::string> LoadDatabaseNames() {
    std::vector<std::string> names;
    std::ifstream input(kDatabaseRegistry);
    std::string name;
    while (std::getline(input, name)) {
        name = Trim(name);
        if (!name.empty() &&
            std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    return names;
}

bool SaveDatabaseNames(const std::vector<std::string>& names) {
    std::ofstream output(kDatabaseRegistry, std::ios::trunc);
    if (!output) return false;
    for (const auto& name : names) output << name << '\n';
    return static_cast<bool>(output);
}

bool IsValidDatabaseName(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char ch : name) {
        if (!std::isalnum(ch) && ch != '_') return false;
    }
    return true;
}

std::string DatabaseFile(const std::string& name) {
    return name + ".db";
}

void PrintDatabaseNames(const std::vector<std::string>& names) {
    ocera::QueryResult result;
    result.column_names = {"Database"};
    for (const auto& name : names) {
        ocera::sql::Value value;
        value.type = ocera::sql::Value::Type::STRING;
        value.string_value = name;
        result.rows.push_back({std::move(value)});
    }
    PrintQueryResult(result);
}

// Database-management statements are shell-level commands for now. They are
// handled before normal SQL dispatch; table SQL continues through SqlExecutor.
bool HandleDatabaseCommand(
    const std::string& statement,
    std::unique_ptr<RuntimeDatabase>& active,
    std::string& active_name,
    std::string& active_file
) {
    std::string text = Trim(statement);
    if (!text.empty() && text.back() == ';') text.pop_back();

    std::istringstream input(text);
    std::string first, second, name, extra;
    if (!(input >> first)) return false;
    first = ToLower(first);
    if (!(input >> second)) return false;
    second = ToLower(second);

    const bool is_create = first == "create" && second == "database";
    const bool is_show = first == "show" && second == "databases";
    const bool is_use = first == "use";
    const bool is_drop = first == "drop" && second == "database";
    if (!is_create && !is_show && !is_use && !is_drop) return false;

    if (is_show) {
        if (input >> extra) {
            std::cout << "ERROR: unexpected extra token '" << extra << "'\n";
            return true;
        }
        PrintDatabaseNames(LoadDatabaseNames());
        return true;
    }

    if (is_use) {
        name = second;
    } else if (!(input >> name)) {
        std::cout << "ERROR: database name is required\n";
        return true;
    }

    if (input >> extra) {
        std::cout << "ERROR: unexpected extra token '" << extra << "'\n";
        return true;
    }
    if (!IsValidDatabaseName(name)) {
        std::cout << "ERROR: invalid database name\n";
        return true;
    }

    const std::string file = DatabaseFile(name);
    auto names = LoadDatabaseNames();

    if (is_create) {
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            std::cout << "ERROR: database '" << name << "' already exists\n";
            return true;
        }
        try {
            RuntimeDatabase created(file, ocera::DatabaseOpenMode::CREATE_NEW);
            created.Flush();
        } catch (const std::exception& e) {
            std::cout << "ERROR: " << e.what() << '\n';
            return true;
        }
        names.push_back(name);
        if (!SaveDatabaseNames(names)) {
            std::remove(file.c_str());
            std::cout << "ERROR: could not update database registry\n";
            return true;
        }
        std::cout << "Database '" << name << "' created successfully.\n";
        return true;
    }

    if (is_use) {
        if (name == active_name) {
            std::cout << "Already using database '" << name << "'.\n";
            return true;
        }
        try {
            auto next = std::make_unique<RuntimeDatabase>(
                file, ocera::DatabaseOpenMode::OPEN_EXISTING);
            active->Flush();
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
                if (!SaveDatabaseNames(names)) {
                    std::cout << "ERROR: could not update database registry; database switch cancelled\n";
                    return true;
                }
            }
            active = std::move(next);
            active_name = name;
            active_file = file;
        } catch (const std::exception& e) {
            std::cout << "ERROR: " << e.what() << '\n';
            return true;
        }
        std::cout << "Switched to database '" << name << "'.\n";
        return true;
    }

    if (is_drop) {
        if (name == active_name) {
            std::cout << "ERROR: cannot drop the currently active database\n";
            return true;
        }
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            std::cout << "ERROR: database '" << name << "' does not exist\n";
            return true;
        }
        if (std::remove(file.c_str()) != 0) {
            std::cout << "ERROR: could not remove database file '" << file << "'\n";
            return true;
        }
        names.erase(std::remove(names.begin(), names.end(), name), names.end());
        if (!SaveDatabaseNames(names)) {
            std::cout << "ERROR: database file removed but registry update failed\n";
            return true;
        }
        std::cout << "Database '" << name << "' dropped successfully.\n";
        return true;
    }
    return false;
}

void ExecuteStatement(
    ocera::SqlExecutor& executor,
    const std::string& statement
) {
    std::string error;
    ocera::RID inserted_rid;
    ocera::QueryResult query_result;

    if (!executor.Execute(
            statement, &inserted_rid, &error, &query_result)) {
        std::cout << "ERROR: " << error << '\n';
        return;
    }

    if (!query_result.column_names.empty()) {
        PrintQueryResult(query_result);
    } else if (inserted_rid.page_id != ocera::INVALID_PAGE_ID) {
        std::cout << "1 row inserted. RID = (page "
                  << inserted_rid.page_id
                  << ", slot "
                  << inserted_rid.slot_num
                  << ")\n";
    } else {
        std::cout << "Statement executed successfully.\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string active_file = (argc > 1) ? argv[1] : "ocera.db";
    std::string active_name = active_file;
    const std::size_t slash = active_name.find_last_of("/\\");
    if (slash != std::string::npos) active_name = active_name.substr(slash + 1);
    const std::size_t dot = active_name.find_last_of('.');
    if (dot != std::string::npos) active_name = active_name.substr(0, dot);

    try {
        auto active = std::make_unique<RuntimeDatabase>(
            active_file, ocera::DatabaseOpenMode::CREATE_OR_OPEN);
        auto names = LoadDatabaseNames();
        if (std::find(names.begin(), names.end(), active_name) == names.end()) {
            names.push_back(active_name);
            SaveDatabaseNames(names);
        }

        ocera::cli::StatementReader reader;
        std::cout << "\n====================================\n";
        std::cout << "          OCERA SQL SHELL\n";
        std::cout << "====================================\n";
        std::cout << "Database: " << active_name << " (" << active_file << ")\n";
        std::cout << "Database commands: CREATE DATABASE, SHOW DATABASES, USE, DROP DATABASE\n";
        std::cout << "Table commands: CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, SHOW TABLES, DESC\n";
        std::cout << "Type .exit to quit.\n\n";

        std::string line;
        while (true) {
            std::cout << (reader.HasPendingInput() ? "...> " : "OCERA> ");
            if (!std::getline(std::cin, line)) {
                if (reader.HasPendingInput())
                    std::cout << "\nIncomplete SQL statement discarded at EOF.\n";
                break;
            }

            const std::string command = ToLower(Trim(line));
            if (!reader.HasPendingInput() &&
                (command == ".exit" || command == ".quit" ||
                 command == "exit" || command == "quit")) break;

            const auto statements = reader.AddLine(line);
            for (const auto& statement : statements) {
                if (Trim(statement).empty()) continue;
                if (HandleDatabaseCommand(statement, active, active_name, active_file))
                    continue;
                ExecuteStatement(active->executor, statement);
            }
        }

        active->Flush();
        std::cout << "\nOCERA closed successfully.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "OCERA startup/runtime error: " << exception.what() << '\n';
        return 1;
    }
}
