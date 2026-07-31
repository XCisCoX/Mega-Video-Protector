#include "videovault/core/version.hpp"

namespace videovault::core {

std::string_view version() noexcept {
    return "0.1.0-phase1";
}

bool is_crypto_available() noexcept {
    // Phase 1 intentionally contains no cryptographic implementation.
    return false;
}

} // namespace videovault::core
