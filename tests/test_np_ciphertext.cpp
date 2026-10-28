// SPDX-License-Identifier: GPL-2.0-or-later
// No PS4 game files or encryption keys required.
// Exercises the same NP ciphertext reader used by the native PKG extractor.
#include "core/file_format/np_ciphertext.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

class MemoryReader {
public:
    explicit MemoryReader(std::vector<std::uint8_t> input) : bytes(std::move(input)) {}

    bool Seek(std::int64_t offset) {
        if (reject_seek || offset < 0 ||
            static_cast<std::uint64_t>(offset) > bytes.size())
            return false;
        cursor = static_cast<std::size_t>(offset);
        return true;
    }

    template <typename T, typename U>
    std::size_t ReadRaw(U* result, std::size_t count) {
        static_assert(sizeof(T) == 1);
        last_read_count = count;
        if (count > bytes.size() - cursor)
            throw std::runtime_error("Unexpected end of test PKG input");
        if (count)
            std::memcpy(result, bytes.data() + cursor, count);
        cursor += count;
        return count;
    }

    std::vector<std::uint8_t> bytes;
    std::size_t cursor = 0;
    std::size_t last_read_count = 0;
    bool reject_seek = false;
};

template <typename Fn>
void MustReject(Fn fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

int main() {
    std::vector<std::uint8_t> source(600);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<std::uint8_t>((i * 37 + 1) % 251);

    // Real Sakura Wars npbind.dat: 532 plaintext bytes, 544 ciphertext
    // bytes including the last 12 encrypted alignment bytes.
    MemoryReader reader(source);
    const auto data = Ps4rtNp::ReadCiphertext(reader, source.size(), 17, 532);
    assert(data.size() == 544);
    assert(reader.last_read_count == 544);
    assert(std::equal(data.begin(), data.end(), source.begin() + 17));
    assert(data[543] == source[17 + 543]);
    assert(data[532] == source[17 + 532]);

    // Already aligned entries must not read into the following PKG entry.
    MemoryReader aligned(source);
    const auto aligned_data = Ps4rtNp::ReadCiphertext(aligned, source.size(), 12, 512);
    assert(aligned_data.size() == 512);
    assert(aligned.last_read_count == 512);

    // Bad lengths/offsets must be rejected before touching the input buffer.
    MemoryReader truncated(source);
    MustReject([&] { (void)Ps4rtNp::ReadCiphertext(truncated, 549, 17, 532); });
    assert(truncated.last_read_count == 0);
    MemoryReader beyond(source);
    MustReject([&] { (void)Ps4rtNp::ReadCiphertext(beyond, 600, 601, 16); });
    MemoryReader overflow(source);
    MustReject([&] { (void)Ps4rtNp::ReadCiphertext(
        overflow, std::numeric_limits<std::uint64_t>::max(), 0,
        std::numeric_limits<std::uint64_t>::max()); });
    MemoryReader no_seek(source);
    no_seek.reject_seek = true;
    MustReject([&] { (void)Ps4rtNp::ReadCiphertext(no_seek, 600, 17, 532); });

    std::cout << "NP AES-aligned ciphertext reader: PASS (532 -> 544, bounds checked)\n";
}
