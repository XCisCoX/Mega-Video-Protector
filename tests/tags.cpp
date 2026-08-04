#include "videovault/core/vault.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVideoProtect_Phase7_tags_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

int fail(const char* message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

videovault::core::Argon2Parameters testParameters() {
    videovault::core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

std::int64_t findTag(
    const std::vector<videovault::core::TagInfo>& tags,
    const std::string& name) {
    for (const auto& tag : tags) {
        if (tag.name == name) {
            return tag.id;
        }
    }
    return -1;
}

bool hasTag(const videovault::core::VideoInfo& video, const std::string& name) {
    for (const auto& tag : video.tags) {
        if (tag == name) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    TemporaryDirectory temp;
    using namespace videovault::core;

    const auto source_one = temp.path() / L"one.bin";
    const auto source_two = temp.path() / L"two.bin";
    std::ofstream one(source_one, std::ios::binary);
    one.write("first import payload", 20);
    one.close();
    std::ofstream two(source_two, std::ios::binary);
    two.write("second import payload", 21);
    two.close();

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "tag test password", testParameters());
    if (!created) {
        return fail("Creating a vault for tag tests must succeed.");
    }
    const auto first_id = created.value().import_file(source_one);
    const auto second_id = created.value().import_file(source_two);
    if (!first_id || !second_id) {
        return fail("Importing the tag test files must succeed.");
    }

    // Tagging, case-insensitive deduplication, and trimming.
    const auto family = created.value().add_tag(first_id.value(), "Family");
    if (!family || family.value() <= 0) {
        return fail("Adding a tag must return a positive tag id.");
    }
    const auto family_again = created.value().add_tag(first_id.value(), "family");
    if (!family_again || family_again.value() != family.value()) {
        return fail("Re-adding a tag must reuse the existing tag id (case-insensitive).");
    }
    const auto trimmed = created.value().add_tag(first_id.value(), "  Vacation  ");
    if (!trimmed || trimmed.value() <= 0) {
        return fail("Tag names must be trimmed of surrounding whitespace.");
    }
    const auto second_family = created.value().add_tag(second_id.value(), "Family");
    if (!second_family || second_family.value() != family.value()) {
        return fail("The same tag must be shared across videos.");
    }

    // Invalid tag names.
    if (created.value().add_tag(first_id.value(), "")  // empty
        || created.value().add_tag(first_id.value(), "   ")) {
        return fail("Empty tag names must be rejected.");
    }
    if (created.value().add_tag(first_id.value(), std::string(65, 'x'))) {
        return fail("Over-long tag names must be rejected.");
    }
    if (created.value().add_tag(first_id.value(), "bad\ntag")) {
        return fail("Tag names with control characters must be rejected.");
    }
    if (created.value().add_tag(999999, "Ghost")) {
        return fail("Tagging an unknown video must return InvalidArgument.");
    }

    // Per-video tag listing.
    {
        const auto tags = created.value().tags_for_video(first_id.value());
        if (!tags) {
            return fail("Listing a video's tags must succeed.");
        }
        if (findTag(tags.value(), "Family") < 0
            || findTag(tags.value(), "Vacation") < 0
            || tags.value().size() != 2U) {
            return fail("The first video must carry exactly Family and Vacation.");
        }
        const auto second_tags = created.value().tags_for_video(second_id.value());
        if (!second_tags || second_tags.value().size() != 1U
            || second_tags.value()[0].name != "Family") {
            return fail("The second video must carry exactly Family.");
        }
    }

    // Global tag listing with counts.
    {
        const auto tags = created.value().list_tags();
        if (!tags) {
            return fail("Listing all tags must succeed.");
        }
        const auto family_count = findTag(tags.value(), "Family");
        const auto vacation_count = findTag(tags.value(), "Vacation");
        if (family_count < 0 || vacation_count < 0) {
            return fail("Both tags must appear in the global list.");
        }
        for (const auto& tag : tags.value()) {
            if (tag.id == family_count && tag.video_count != 2) {
                return fail("Family must be counted on two videos.");
            }
            if (tag.id == vacation_count && tag.video_count != 1) {
                return fail("Vacation must be counted on one video.");
            }
        }
    }

    // Videos carry their tags in the gallery listing.
    {
        const auto videos = created.value().list_videos();
        if (!videos) {
            return fail("Listing videos must succeed.");
        }
        for (const auto& video : videos.value()) {
            if (video.id == first_id.value()) {
                if (!hasTag(video, "Family") || !hasTag(video, "Vacation")) {
                    return fail("The gallery entry must expose the video's tags.");
                }
            } else if (video.id == second_id.value()) {
                if (!hasTag(video, "Family") || hasTag(video, "Vacation")) {
                    return fail("The second gallery entry must expose only Family.");
                }
            }
        }
    }

    // Untagging.
    {
        const auto tags = created.value().tags_for_video(first_id.value());
        const auto family_id = findTag(tags.value(), "Family");
        if (family_id < 0) {
            return fail("Family must exist before untagging.");
        }
        const auto removed = created.value().remove_tag(first_id.value(), family_id);
        if (!removed || !removed.value()) {
            return fail("Untagging must succeed.");
        }
        const auto after = created.value().tags_for_video(first_id.value());
        if (!after || after.value().size() != 1U
            || after.value()[0].name != "Vacation") {
            return fail("Untagging must remove only the chosen tag.");
        }
    }

    // Removal cascades the video-tag associations (the tag itself stays).
    {
        const auto removed = created.value().remove_video(second_id.value());
        if (!removed || !removed.value()) {
            return fail("Removing the tagged video must succeed.");
        }
        const auto tags = created.value().list_tags();
        const auto family_id = findTag(tags.value(), "Family");
        if (family_id < 0) {
            return fail("The tag must survive the video removal.");
        }
        for (const auto& tag : tags.value()) {
            if (tag.id == family_id && tag.video_count != 0) {
                return fail("The removed video's tag association must cascade away.");
            }
        }
    }

    // Tags persist across reopen, and locked vaults reject tag operations.
    {
        created.value().lock();
        auto reopened = Vault::open(root, "tag test password");
        if (!reopened) {
            return fail("The vault must reopen with tags intact.");
        }
        const auto tags = reopened.value().tags_for_video(first_id.value());
        if (!tags || tags.value().size() != 1U
            || tags.value()[0].name != "Vacation") {
            return fail("Tags must survive a reopen.");
        }
        const auto locked_add = created.value().add_tag(first_id.value(), "Blocked");
        if (locked_add || locked_add.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Tagging on a locked vault must fail cleanly.");
        }
        reopened.value().lock();
    }

    std::cout << "Tag checks succeeded.\n";
    return EXIT_SUCCESS;
}
