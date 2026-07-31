#include "videovault/core/version.hpp"

namespace videovault::core {

std::string_view version() noexcept {
    return "0.2.0-phase2";
}

bool is_crypto_available() noexcept {
    return true;
}

} // namespace videovault::core
