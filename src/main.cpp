#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/validator.h"
#include "cli/statement_reader.h"
#include "sql/parser.h"
#include "sql/tokenizer.h"

namespace {

int shared_counter = 0;
std::mutex counter_mutex;

void IncrementManyTimes(int times) {
    for (int i = 0; i < times; ++i) {
        std::lock_guard<std::mutex> lock(counter_mutex);
        ++shared_counter;
    }
}

int RunThreadCheck() {
    std::cout << "OCERA threading environment check starting...\n";

    shared_counter = 0;

    constexpr int num_threads = 4;
    constexpr int increments_per_thread = 50000;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(IncrementManyTimes, increments_per_thread);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const int expected = num_threads * increments_per_thread;

    std::cout << "Final counter value: " << shared_counter << '\n';
    std::cout << "Expected value:      " << expected << '\n';

    if (shared_counter == expected) {
        std::cout << "Environment OK - threads + mutex working correctly.\n";
        return 0;
    }

    std::cout << "Mismatch! Check the build/threading setup.\n";
    return 1;
}

void ProcessSqlStatement(
    const std::string& sql,
    ocera::Catalog& catalog
) {
    try {
        const ocera::sql::Statement statement = ocera::sql::Parse(sql);

        // Check the statement against the current in-memory Catalog.
        ocera::Validate(statement, catalog);

        // CREATE TABLE registers its schema in the Catalog.
        if (const auto* create =
                std::get_if<ocera::sql::CreateTableStatement>(&statement)) {
            catalog.CreateTable(*create);

            std::cout << "Table '" << create->table
                      << "' registered in the in-memory Catalog.\n";
        } else {
            std::cout << "Semantic validation passed.\n";
            std::cout << "SQL execution is not implemented yet.\n";
        }
    } catch (const ocera::SemanticError& error) {
        std::cout << "Semantic error: " << error.what() << '\n';
    } catch (const ocera::CatalogError& error) {
        std::cout << "Catalog error: " << error.what() << '\n';
    } catch (const ocera::sql::TokenizerError& error) {
        std::cout << "Lexical error: " << error.what() << '\n';
    } catch (const ocera::sql::ParseError& error) {
        std::cout << "Syntax error: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cout << "Error: " << error.what() << '\n';
    }
}

int RunSqlCli() {
    std::string input;
    ocera::Catalog catalog;
    ocera::cli::StatementReader reader;

    std::cout << "OCERA SQL CLI (parse-only mode)\n";
    std::cout << "Enter SQL statements ending with ';', or type exit to quit.\n";
    std::cout << "SQL execution is not implemented yet.\n\n";

    while (true) {
        if (reader.HasPendingInput()) {
            std::cout << "...> " << std::flush;
        } else {
            std::cout << "OCERA> " << std::flush;
        }

        if (!std::getline(std::cin, input)) {
            if (reader.HasPendingInput()) {
                std::cout << "\nIncomplete SQL statement discarded "
                             "(missing semicolon).\n";
            }

            std::cout << "Exiting OCERA CLI.\n";
            break;
        }

        // Ignore blank input when no statement is being collected.
        const std::size_t first = input.find_first_not_of(" \t\r\n");

        if (first == std::string::npos) {
            if (reader.HasPendingInput()) {
                reader.AddLine(input);
            }
            continue;
        }

        const std::size_t last = input.find_last_not_of(" \t\r\n");
        const std::string command = input.substr(first, last - first + 1);

        // Exit commands are recognized only when no SQL is pending.
        if (!reader.HasPendingInput() &&
            (command == "exit" || command == ".exit")) {
            std::cout << "Exiting OCERA CLI.\n";
            break;
        }

        // One input line can complete multiple SQL statements.
        const std::vector<std::string> statements = reader.AddLine(input);

        for (const std::string& statement : statements) {
            ProcessSqlStatement(statement, catalog);
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--thread-check") {
        return RunThreadCheck();
    }

    if (argc != 1) {
        std::cerr << "Usage: ocera [--thread-check]\n";
        return 1;
    }

    return RunSqlCli();
}