// SPDX-License-Identifier: GPL-2.0-or-later
// Read the full AES-CBC ciphertext for PS4 NP metadata entries.
// The PKG table records the unpadded payload size, but the encrypted
// entry occupies complete 16-byte blocks on disk.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Ps4rtNp {

inline std::size_t CheckedCiphertextSize(std::uint64_t payload_size,
                                         std::uint64_t pkg_size,
                                         std::uint64_t offset) {
    constexpr std::uint64_t block_size = 16;
    if (payload_size > std::numeric_limits<std::uint64_t>::max() - (block_size - 1))
        throw std::runtime_error("NP metadata ciphertext length overflow");
    const std::uint64_t padded =
        (payload_size + block_size - 1) & ~(block_size - 1);
    if (offset > pkg_size || padded > pkg_size - offset)
        throw std::runtime_error("NP metadata ciphertext extends beyond PKG input");
    if (padded > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("NP metadata ciphertext exceeds addressable memory");
    return static_cast<std::size_t>(padded);
}

template <typename Reader>
std::vector<std::uint8_t> ReadCiphertext(Reader& file,
                                         std::uint64_t pkg_size,
                                         std::uint64_t offset,
                                         std::uint64_t payload_size) {
    const std::size_t count = CheckedCiphertextSize(payload_size, pkg_size, offset);
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !file.Seek(static_cast<std::int64_t>(offset)))
        throw std::runtime_error("Failed to seek to NP metadata entry offset");
    std::vector<std::uint8_t> ciphertext(count);
    file.template ReadRaw<std::uint8_t>(ciphertext.data(), count);
    return ciphertext;
}

} // namespace Ps4rtNp
