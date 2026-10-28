// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal standalone stdio adapter for the vendored shadPS4 PKG parser.
// This avoids bringing the emulator's logging, Qt, and IO framework into PS4RT.
#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace Common::FS {

enum class FileAccessMode { Read, Write };
enum class SeekOrigin { SetOrigin, CurrentPosition, End };

class IOFile {
public:
    IOFile() = default;
    explicit IOFile(const std::filesystem::path& path, FileAccessMode mode) {
        Open(path, mode);
    }
    IOFile(const IOFile&) = delete;
    IOFile& operator=(const IOFile&) = delete;
    ~IOFile() { Close(); }

    bool Open(const std::filesystem::path& path, FileAccessMode mode) {
        Close();
        stream_ = std::fopen(path.c_str(), mode == FileAccessMode::Read ? "rb" : "wb");
        if (stream_) path_ = path;
        return stream_ != nullptr;
    }
    bool IsOpen() const { return stream_ != nullptr; }
    void Close() {
        if (stream_) {
            std::fclose(stream_);
            stream_ = nullptr;
        }
    }
    bool Seek(std::int64_t pos, SeekOrigin from = SeekOrigin::SetOrigin) {
        if (!stream_) return false;
        const int origin = from == SeekOrigin::SetOrigin ? SEEK_SET :
                           from == SeekOrigin::CurrentPosition ? SEEK_CUR : SEEK_END;
        return ::fseeko(stream_, static_cast<off_t>(pos), origin) == 0;
    }
    std::uint64_t Tell() const {
        if (!stream_) return 0;
        const auto value = ::ftello(stream_);
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }
    std::uint64_t GetSize() {
        if (!stream_) return 0;
        const auto cursor = ::ftello(stream_);
        if (cursor < 0 || ::fseeko(stream_, 0, SEEK_END) != 0) return 0;
        const auto size = ::ftello(stream_);
        ::fseeko(stream_, cursor, SEEK_SET);
        return size < 0 ? 0 : static_cast<std::uint64_t>(size);
    }
    template <typename T, typename U> std::size_t ReadRaw(U* data, std::size_t count) {
        if (!stream_ || (!data && count))
            throw std::runtime_error("PKG read attempted on an unopened file");
        const auto offset = ::ftello(stream_);
        const auto received = std::fread(data, sizeof(T), count, stream_);
        if (received != count) {
            const bool io_error = std::ferror(stream_) != 0;
            throw std::runtime_error(
                std::string(io_error ? "PKG input I/O error" : "Short PS4 PKG read") +
                " at byte " + std::to_string(offset) +
                ": wanted " + std::to_string(count * sizeof(T)) +
                " bytes, received " + std::to_string(received * sizeof(T)) +
                ", file size " + std::to_string(GetSize()) +
                " (" + path_.string() + ")");
        }
        return received;
    }
    template <typename T, typename U> std::size_t WriteRaw(const U* data, std::size_t count) {
        if (!stream_ || (!data && count)) return 0;
        const auto written = std::fwrite(data, sizeof(T), count, stream_);
        if (written != count) throw std::runtime_error("Short write in PS4 PKG extraction");
        return written;
    }
    template <typename T> std::size_t Read(T& value) {
        return ReadRaw<T>(&value, 1);
    }
    template <typename T, std::size_t N> std::size_t Read(std::array<T, N>& value) {
        return ReadRaw<T>(value.data(), N);
    }
    template <typename T, std::size_t N> std::size_t Read(T (&value)[N]) {
        return ReadRaw<T>(value, N);
    }
    template <typename T> std::size_t Read(std::vector<T>& value) {
        return ReadRaw<T>(value.data(), value.size());
    }
    template <typename T> std::size_t Write(const std::vector<T>& value) {
        return WriteRaw<T>(value.data(), value.size());
    }

private:
    std::FILE* stream_ = nullptr;
    std::filesystem::path path_;
};
} // namespace Common::FS
