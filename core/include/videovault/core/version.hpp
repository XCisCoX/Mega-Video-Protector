#pragma once

#include <string_view>

namespace videovault::core {

[[nodiscard]] std::string_view version() noexcept;
[[nodiscard]] bool is_crypto_available() noexcept;

} // namespace videovault::core
