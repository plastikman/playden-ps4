#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace playden::steam {

// Implausible-size guard for one uncompressed depot chunk (chunks are <= 1 MiB in practice).
inline constexpr std::size_t kMaxDepotChunkBytes = 64 * 1024 * 1024;

enum class ChunkError { kNone, kDecrypt, kFormat, kDecompress, kSize, kChecksum };

// Steam symmetric scheme: first 16 bytes = AES-256-ECB-encrypted IV, rest AES-256-CBC with PKCS7.
bool SymmetricDecrypt(const std::uint8_t* data, std::size_t size, const std::array<std::uint8_t, 32>& key,
                      std::vector<std::uint8_t>* out);

// Manifest chunk checksum: Adler-32 with a zero initial seed (a = b = 0), not the zlib seed of 1.
std::uint32_t DepotChunkAdler32(const std::uint8_t* data, std::size_t size);

// Decompresses a decrypted chunk by magic: "VSZ" (zstd), "VZ" (LZMA1), "PK" (zip, first entry stored/deflate).
// No size or checksum verification beyond what the container itself declares.
ChunkError DecodeDepotChunk(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>* out);

// Decrypts, decompresses and verifies one depot chunk against its manifest entry. expected_size is the
// manifest's cb_original. When expected_checksum (manifest crc) is non-zero the data must match its
// Adler-32; when it is zero the data must match chunk_sha (SHA-1 of the uncompressed chunk).
ChunkError ProcessDepotChunk(const std::vector<std::uint8_t>& encrypted, const std::array<std::uint8_t, 32>& key,
                             std::size_t expected_size, std::uint32_t expected_checksum,
                             const std::array<std::uint8_t, 20>& chunk_sha, std::vector<std::uint8_t>* out);
// The install path owns the downloaded buffer and can decrypt it in place. This avoids holding
// a second compressed-size buffer while allocating the uncompressed chunk on PS4.
ChunkError ProcessDepotChunkInPlace(std::vector<std::uint8_t>* encrypted,
                                   const std::array<std::uint8_t, 32>& key,
                                   std::size_t expected_size, std::uint32_t expected_checksum,
                                   const std::array<std::uint8_t, 20>& chunk_sha,
                                   std::vector<std::uint8_t>* out);

}  // namespace playden::steam
