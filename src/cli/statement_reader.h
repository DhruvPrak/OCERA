#pragma once

#include <string>
#include <vector>

namespace ocera {
namespace cli {

class StatementReader {
public:
    // Adds one input line and returns all complete SQL statements.
    std::vector<std::string> AddLine(const std::string& line) {
        std::vector<std::string> statements;

        for (char ch : line) {
            // We saw a quote inside a string. Check whether this
            // quote is escaped by another single quote.
            if (inside_string_ && quote_pending_) {
                if (ch == '\'') {
                    buffer_ += ch;
                    quote_pending_ = false;
                    continue;
                }

                // The previous quote closed the string.
                inside_string_ = false;
                quote_pending_ = false;
            }

            buffer_ += ch;

            if (inside_string_) {
                if (ch == '\'') {
                    quote_pending_ = true;
                }
            } else if (ch == '\'') {
                inside_string_ = true;
            } else if (ch == ';') {
                statements.push_back(buffer_);
                buffer_.clear();
            }
        }

        // A line break separates adjacent SQL characters.
        // A quote at the end of this line therefore closes the string.
        if (quote_pending_) {
            inside_string_ = false;
            quote_pending_ = false;
        }

        if (!buffer_.empty()) {
            buffer_ += '\n';
        }

        return statements;
    }

    bool HasPendingInput() const {
        return buffer_.find_first_not_of(" \t\r\n") != std::string::npos;
    }

private:
    std::string buffer_;
    bool inside_string_ = false;
    bool quote_pending_ = false;
};

}  // namespace cli
}  // namespace ocera