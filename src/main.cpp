
#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "cli/statement_reader.h"
#include "sql/executor.h"
#include "storage/buffer_pool_manager.h"
#include "storage/database_header.h"
#include "storage/disk_manager.h"
#include "storage/free_space_manager.h"

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
    const std::string db_file =
        (argc > 1) ? argv[1] : "ocera.db";

    try {
        ocera::DiskManager disk_manager(db_file);
        ocera::FreeSpaceManager free_space_manager(disk_manager);
        ocera::DatabaseHeader database_header(disk_manager);

        ocera::BufferPoolManager bpm(
            64, disk_manager, free_space_manager
        );

        ocera::Catalog catalog;
        ocera::SqlExecutor executor(catalog, bpm);
        ocera::cli::StatementReader reader;

        std::cout << "\n";
        std::cout << "====================================\n";
        std::cout << "          OCERA SQL SHELL\n";
        std::cout << "====================================\n";
        std::cout << "Database file: " << db_file << '\n';
        std::cout << "Supported SQL: CREATE TABLE, INSERT, SELECT, UPDATE, DELETE\n";
        std::cout << "Type .exit to quit.\n";
        std::cout << "Note: Table catalog metadata is currently "
                     "session-only.\n\n";

        std::string line;
        bool running = true;

        while (running) {
            std::cout
                << (reader.HasPendingInput() ? "...> " : "OCERA> ");

            if (!std::getline(std::cin, line)) {
                if (reader.HasPendingInput()) {
                    std::cout
                        << "\nIncomplete SQL statement discarded at EOF.\n";
                }
                break;
            }

            const std::string command = ToLower(Trim(line));

            if (!reader.HasPendingInput() &&
                (command == ".exit" || command == ".quit" ||
                 command == "exit" || command == "quit")) {
                running = false;
                continue;
            }

            const std::vector<std::string> statements =
                reader.AddLine(line);

            for (const auto& statement : statements) {
                if (!Trim(statement).empty()) {
                    ExecuteStatement(executor, statement);
                }
            }
        }

        bpm.FlushAllPages();

        std::cout << "\nOCERA closed successfully.\n";
        return 0;

    } catch (const std::exception& exception) {
        std::cerr << "OCERA startup/runtime error: "
                  << exception.what() << '\n';
        return 1;
    }
}
