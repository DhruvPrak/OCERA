#pragma once

#include <array>
#include <cstdint>

namespace ocera {

// Stable 128-bit identity shared by a database file and its WAL.
using DatabaseId = std::array<std::uint8_t, 16>;

}  // namespace ocera
