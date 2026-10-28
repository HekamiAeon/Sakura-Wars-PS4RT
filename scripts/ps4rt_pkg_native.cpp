// SPDX-License-Identifier: GPL-2.0-or-later
// PS4RT-native player PKG extraction entry point.
// Adapted to use the shadPS4Plus PKG/PFS implementation (PKG_EXTRACTOR_1_1).
// Unlike the reference executable, errors always produce a nonzero exit code.
// Usage: ps4rt-pkg INPUT.pkg EXISTING_PRIVATE_STAGING_DIRECTORY
#include "core/file_format/pkg.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

static std::uint16_t le16(const std::vector<u8>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 2)
        throw std::runtime_error("Invalid param.sfo: truncated 16-bit field");
    return std::uint16_t(data[offset]) | (std::uint16_t(data[offset + 1]) << 8);
}
static std::uint32_t le32(const std::vector<u8>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4)
        throw std::runtime_error("Invalid param.sfo: truncated 32-bit field");
    return std::uint32_t(data[offset]) | (std::uint32_t(data[offset+1]) << 8) |
           (std::uint32_t(data[offset+2]) << 16) | (std::uint32_t(data[offset+3]) << 24);
}
static std::string sfo_string(const std::vector<u8>& data, std::string_view name) {
    if (data.size() < 20 || std::memcmp(data.data(), "\0PSF", 4) != 0)
        throw std::runtime_error("Invalid PKG param.sfo magic");
    const size_t keys = le32(data, 8);
    const size_t values = le32(data, 12);
    const size_t count = le32(data, 16);
    if (count > 2048 || keys > data.size() || values > data.size() ||
        count > (data.size() - 20) / 16)
        throw std::runtime_error("Invalid PKG param.sfo index");
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = 20 + 16 * i;
        const size_t key = keys + le16(data, entry);
        const size_t value = values + le32(data, entry + 12);
        const size_t length = le32(data, entry + 4);
        if (key >= data.size() || value > data.size() ||
            length > data.size() - value)
            throw std::runtime_error("Invalid PKG param.sfo string bounds");
        const char* key_ptr = reinterpret_cast<const char*>(data.data() + key);
        const char* key_end = reinterpret_cast<const char*>(data.data() + data.size());
        const char* terminator = static_cast<const char*>(
            std::memchr(key_ptr, 0, static_cast<size_t>(key_end - key_ptr)));
        if (!terminator) throw std::runtime_error("Unterminated PKG param.sfo key");
        if (std::string_view(key_ptr, static_cast<size_t>(terminator - key_ptr)) == name) {
            const auto* raw = reinterpret_cast<const char*>(data.data() + value);
            const auto* end = static_cast<const char*>(std::memchr(raw, 0, length));
            return std::string(raw, end ? end : raw + length);
        }
    }
    return {};
}
static bool valid_cusa(std::string_view value) {
    if (value.size() != 9 || value.substr(0, 4) != "CUSA") return false;
    for (size_t i = 4; i < value.size(); ++i)
        if (value[i] < '0' || value[i] > '9') return false;
    return true;
}
static bool valid_label(std::string_view label) {
    if (label.size() != 16) return false;
    for (unsigned char c : label)
        if (!std::isalnum(c)) return false;
    return true;
}
static int extract(const fs::path& pkg_path, const fs::path& staging) {
    if (!fs::is_regular_file(pkg_path) || pkg_path.extension() != ".pkg")
        throw std::runtime_error("Expected a readable .pkg file");
    if (!fs::is_directory(staging) || fs::is_symlink(staging))
        throw std::runtime_error("Private extraction staging directory is missing");

    PKG pkg;
    std::string reason;
    if (!pkg.Open(pkg_path, reason))
        throw std::runtime_error("Could not read PS4 PKG header: " + reason);
    const std::string title(pkg.GetTitleID());
    const std::string sfo_title = sfo_string(pkg.sfo, "TITLE_ID");
    const std::string content = sfo_string(pkg.sfo, "CONTENT_ID");
    const std::string category = sfo_string(pkg.sfo, "CATEGORY");
    if (!valid_cusa(title) || title != sfo_title)
        throw std::runtime_error("PKG header and param.sfo TITLE_ID disagree");
    if (content.size() != 36 || content.substr(7, 9) != title)
        throw std::runtime_error("Invalid PKG CONTENT_ID / title");
    if (category != "gp" && category != "ac" && category != "ad")
        throw std::runtime_error("Only game update and DLC PKGs are accepted");
    if (category == "gp" && pkg.GetPkgFlags().find("PATCH") == std::string::npos)
        throw std::runtime_error("Update PKG does not have a patch flag");

    const bool is_patch = category == "gp";
    const std::string label = content.substr(20, 16);
    if (!is_patch && !valid_label(label))
        throw std::runtime_error("Invalid DLC entitlement label");
    const fs::path actual = is_patch ? staging / (title + "-patch") :
                                     staging / title / label;
    if (fs::exists(actual))
        throw std::runtime_error("Refusing to overwrite existing extraction");

    std::cout << "PS4RT PKG: " << (is_patch ? "update " : "DLC ") << content << '\n'
              << "Extracting into PS4RT private staging...\n" << std::flush;
    if (!pkg.Extract(pkg_path, actual, reason))
        throw std::runtime_error("PKG filesystem extraction failed: " + reason);
    const auto count = pkg.GetNumberOfFiles();
    if (count == 0)
        throw std::runtime_error("The PKG yielded no filesystem entries");
    for (std::uint32_t i = 0; i < count; ++i) {
        pkg.ExtractFiles(i);
        if ((i + 1) % 25 == 0 || i + 1 == count)
            std::cout << "PS4RT PKG: " << (i + 1) << "/" << count
                      << " entries processed\n" << std::flush;
    }
    if (!fs::is_regular_file(actual / "sce_sys/param.sfo"))
        throw std::runtime_error("PKG output lacks sce_sys/param.sfo");
    if (is_patch) {
        if (!fs::is_regular_file(actual / "eboot.bin"))
            throw std::runtime_error("PKG update output lacks required eboot.bin");
    } else {
        // Present a single-level add-on folder to player_setup.find_addons().
        const fs::path final = staging / label;
        if (fs::exists(final))
            throw std::runtime_error("Unexpected existing DLC output directory");
        fs::rename(actual, final);
        fs::remove(staging / title); // empty directory only
    }
    std::cout << "PS4RT PKG: extraction finished and validated\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: ps4rt-pkg INPUT.pkg PRIVATE_STAGING_DIR\n";
        return 2;
    }
    try {
        return extract(fs::path(argv[1]), fs::path(argv[2]));
    } catch (const std::exception& error) {
        std::cerr << "PS4RT PKG extraction failed: " << error.what() << '\n';
        return 1;
    }
}
