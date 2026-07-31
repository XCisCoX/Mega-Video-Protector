#include "videovault/core/version.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    if (videovault::core::version().empty()) {
        std::cerr << "Core version must not be empty.\n";
        return EXIT_FAILURE;
    }
    if (videovault::core::is_crypto_available()) {
        std::cerr << "Phase 1 must not advertise unavailable cryptography.\n";
        return EXIT_FAILURE;
    }

    std::cout << "VideoVaultCore " << videovault::core::version()
              << " linked successfully.\n";
    return EXIT_SUCCESS;
}
