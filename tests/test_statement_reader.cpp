#include <iostream>
#include <string>
#include <vector>

#include "cli/statement_reader.h"

int failures = 0;

void Check(
    const std::string& name,
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected
) {
    if (actual == expected) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cout << "[FAIL] " << name << '\n';
        ++failures;
    }
}

int main() {
    {
        ocera::cli::StatementReader reader;
        Check("Simple statement",
              reader.AddLine("SELECT * FROM users;"),
              {"SELECT * FROM users;"});
    }

    {
        ocera::cli::StatementReader reader;
        Check("Multiline statement",
              reader.AddLine("SELECT *\nFROM users;"),
              {"SELECT *\nFROM users;"});
    }

    {
        ocera::cli::StatementReader reader;
        Check("Semicolon inside string",
              reader.AddLine("INSERT INTO users VALUES ('hello; world');"),
              {"INSERT INTO users VALUES ('hello; world');"});
    }

    {
        ocera::cli::StatementReader reader;
        Check("Escaped quote",
              reader.AddLine("INSERT INTO users VALUES ('It''s working');"),
              {"INSERT INTO users VALUES ('It''s working');"});
    }

    {
        ocera::cli::StatementReader reader;
        Check("Multiple statements",
              reader.AddLine("SELECT * FROM users; DELETE FROM users;"),
              {"SELECT * FROM users;", " DELETE FROM users;"});
    }

    {
        ocera::cli::StatementReader reader;
        reader.AddLine("SELECT * FROM users");
        if (reader.HasPendingInput()) {
            std::cout << "[PASS] Incomplete statement remains pending\n";
        } else {
            std::cout << "[FAIL] Incomplete statement remains pending\n";
            ++failures;
        }
    }

    {
        ocera::cli::StatementReader reader;
        reader.AddLine("SELECT * FROM users");
        Check("Statement completed on next line",
              reader.AddLine("WHERE id = 1;"),
              {"SELECT * FROM users\nWHERE id = 1;"});
    }

    std::cout << "\nFailures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
