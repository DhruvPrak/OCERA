#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include "sql/ast.h"

namespace ocera {
namespace sql {
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& message, std::size_t position)
        : std::runtime_error(message + " (at position " +
                             std::to_string(position) + ")"),
          position_(position) {}
    std::size_t position() const { return position_; }
private:
    std::size_t position_;
};

Statement Parse(const std::string& sql);
}  // namespace sql
}  // namespace ocera
