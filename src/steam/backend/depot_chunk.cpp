#include "steam/backend/depot_chunk.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>

extern "C" {
#include "LzmaDec.h"
#include "zstd.h"
}

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_LINEAR
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace playden::steam {
namespace {

constexpr std::size_t kAesBlock = 16;

// "VZ": 'V' 'Z' 'a' crc32(4) | lzma props(5) + payload | crc32(4) size(4) 'z' 'v'.
constexpr std::size_t kVzHeader = 7;
constexpr std::size_t kVzProps = 5;
constexpr std::size_t kVzFooter = 10;
// "VSZ": 'V' 'S' 'Z' 'a' crc32(4) | zstd frame | crc32(4) size(8) 'z' 's' 'v'.
constexpr std::size_t kVszHeader = 8;
constexpr std::size_t kVszFooter = 15;

std::uint32_t ReadLe16(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U);
}

std::uint32_t ReadLe32(const std::uint8_t* p) { return ReadLe16(p) | (ReadLe16(p + 2) << 16U); }

std::uint64_t ReadLe64(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(ReadLe32(p)) | (static_cast<std::uint64_t>(ReadLe32(p + 4)) << 32U);
}

void* LzmaAlloc(ISzAllocPtr, std::size_t size) { return std::malloc(size); }
void LzmaFree(ISzAllocPtr, void* address) { std::free(address); }

ChunkError DecodeVz(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>* out) {
  if (n <= kVzHeader + kVzProps + kVzFooter) return ChunkError::kFormat;
  if (d[0] != 'V' || d[1] != 'Z' || d[2] != 'a') return ChunkError::kFormat;
  if (d[n - 2] != 'z' || d[n - 1] != 'v') return ChunkError::kFormat;
  const std::size_t size = ReadLe32(d + n - 6);
  if (size > kMaxDepotChunkBytes) return ChunkError::kSize;
  if (size == 0) return ChunkError::kNone;
  const std::uint8_t* props = d + kVzHeader;
  const std::uint8_t* payload = props + kVzProps;
  const std::size_t payload_size = n - kVzFooter - kVzHeader - kVzProps;

  // Steam's LZMA1 streams carry no end marker (the size comes from the footer): decode with
  // LZMA_FINISH_ANY and accept once the output buffer is full, as the reference does.
  std::vector<std::uint8_t> result(size);
  SizeT dest_len = size;
  SizeT src_len = payload_size;
  ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
  const ISzAlloc alloc = {LzmaAlloc, LzmaFree};
  const SRes res = LzmaDecode(result.data(), &dest_len, payload, &src_len, props, kVzProps, LZMA_FINISH_ANY,
                              &status, &alloc);
  if (res != SZ_OK || dest_len != size) return ChunkError::kDecompress;
  *out = std::move(result);
  return ChunkError::kNone;
}

ChunkError DecodeVsz(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>* out) {
  if (n <= kVszHeader + kVszFooter) return ChunkError::kFormat;
  if (d[0] != 'V' || d[1] != 'S' || d[2] != 'Z' || d[3] != 'a') return ChunkError::kFormat;
  if (d[n - 3] != 'z' || d[n - 2] != 's' || d[n - 1] != 'v') return ChunkError::kFormat;
  const std::uint64_t size = ReadLe64(d + n - kVszFooter + 4);
  if (size > kMaxDepotChunkBytes) return ChunkError::kSize;
  if (size == 0) return ChunkError::kNone;
  std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
  const std::size_t decoded =
      ZSTD_decompress(result.data(), result.size(), d + kVszHeader, n - kVszHeader - kVszFooter);
  if (ZSTD_isError(decoded) != 0 || decoded != result.size()) return ChunkError::kDecompress;
  *out = std::move(result);
  return ChunkError::kNone;
}

// First entry of a zip archive via the central directory; stored or raw deflate only.
ChunkError DecodeZip(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>* out) {
  constexpr std::size_t kEocdSize = 22;
  constexpr std::size_t kCentralSize = 46;
  constexpr std::size_t kLocalSize = 30;
  if (n < kEocdSize) return ChunkError::kFormat;
  std::size_t eocd = n;  // n means "not found"
  const std::size_t last = n - kEocdSize;
  const std::size_t floor = last > 0xFFFF ? last - 0xFFFF : 0;
  for (std::size_t i = last + 1; i-- > floor;) {
    if (d[i] == 'P' && d[i + 1] == 'K' && d[i + 2] == 5 && d[i + 3] == 6) {
      eocd = i;
      break;
    }
  }
  if (eocd == n) return ChunkError::kFormat;
  const std::size_t cd = ReadLe32(d + eocd + 16);
  if (cd > n || n - cd < kCentralSize || ReadLe32(d + cd) != 0x02014B50U) return ChunkError::kFormat;
  const std::uint32_t method = ReadLe16(d + cd + 10);
  const std::size_t csize = ReadLe32(d + cd + 20);
  const std::size_t usize = ReadLe32(d + cd + 24);
  const std::size_t local = ReadLe32(d + cd + 42);
  if (local > n || n - local < kLocalSize || ReadLe32(d + local) != 0x04034B50U) return ChunkError::kFormat;
  const std::size_t data_start = local + kLocalSize + ReadLe16(d + local + 26) + ReadLe16(d + local + 28);
  if (data_start > n || n - data_start < csize) return ChunkError::kFormat;
  if (usize > kMaxDepotChunkBytes) return ChunkError::kSize;
  const std::uint8_t* payload = d + data_start;
  if (method == 0) {
    if (csize != usize) return ChunkError::kFormat;
    out->assign(payload, payload + csize);
    return ChunkError::kNone;
  }
  if (method != 8) return ChunkError::kFormat;
  if (usize == 0) return ChunkError::kNone;
  if (csize > static_cast<std::size_t>(INT_MAX) || usize > static_cast<std::size_t>(INT_MAX)) {
    return ChunkError::kSize;
  }
  std::vector<std::uint8_t> result(usize);
  const int written = stbi_zlib_decode_noheader_buffer(
      reinterpret_cast<char*>(result.data()), static_cast<int>(usize), reinterpret_cast<const char*>(payload),
      static_cast<int>(csize));
  if (written < 0 || static_cast<std::size_t>(written) != usize) return ChunkError::kDecompress;
  *out = std::move(result);
  return ChunkError::kNone;
}

}  // namespace

bool SymmetricDecrypt(const std::uint8_t* data, std::size_t size, const std::array<std::uint8_t, 32>& key,
                      std::vector<std::uint8_t>* out) {
  if (out == nullptr) return false;
  out->clear();
  if (data == nullptr || size <= kAesBlock || (size % kAesBlock) != 0) return false;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  bool ok = false;
  std::vector<std::uint8_t> plain(size - kAesBlock);
  std::uint8_t iv[kAesBlock];
  if (mbedtls_aes_setkey_dec(&ctx, key.data(), 256) == 0 &&
      mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, data, iv) == 0 &&
      mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, plain.size(), iv, data + kAesBlock, plain.data()) == 0) {
    const std::uint8_t pad = plain.back();
    if (pad >= 1 && pad <= kAesBlock && pad <= plain.size()) {
      ok = true;
      for (std::size_t i = plain.size() - pad; i < plain.size(); ++i) {
        if (plain[i] != pad) ok = false;
      }
      if (ok) plain.resize(plain.size() - pad);
    }
  }
  mbedtls_aes_free(&ctx);
  if (!ok) return false;
  *out = std::move(plain);
  return true;
}

std::uint32_t DepotChunkAdler32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  for (std::size_t i = 0; i < size; ++i) {
    a = (a + data[i]) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16U) | a;
}

ChunkError DecodeDepotChunk(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>* out) {
  if (out == nullptr) return ChunkError::kFormat;
  out->clear();
  if (data == nullptr) return ChunkError::kFormat;
  // 'VSZ' before 'VZ': both start with 'V', only the second byte differs.
  if (size >= 3 && data[0] == 'V' && data[1] == 'S' && data[2] == 'Z') return DecodeVsz(data, size, out);
  if (size >= 2 && data[0] == 'V' && data[1] == 'Z') return DecodeVz(data, size, out);
  if (size >= 2 && data[0] == 'P' && data[1] == 'K') return DecodeZip(data, size, out);
  return ChunkError::kFormat;
}

ChunkError ProcessDepotChunk(const std::vector<std::uint8_t>& encrypted, const std::array<std::uint8_t, 32>& key,
                             std::size_t expected_size, std::uint32_t expected_checksum,
                             const std::array<std::uint8_t, 20>& chunk_sha, std::vector<std::uint8_t>* out) {
  if (out == nullptr) return ChunkError::kFormat;
  out->clear();
  std::vector<std::uint8_t> decrypted;
  if (!SymmetricDecrypt(encrypted.data(), encrypted.size(), key, &decrypted)) return ChunkError::kDecrypt;
  std::vector<std::uint8_t> data;
  const ChunkError decoded = DecodeDepotChunk(decrypted.data(), decrypted.size(), &data);
  if (decoded != ChunkError::kNone) return decoded;
  if (data.size() != expected_size) return ChunkError::kSize;
  if (expected_checksum != 0) {
    if (DepotChunkAdler32(data.data(), data.size()) != expected_checksum) return ChunkError::kChecksum;
  } else {
    std::array<std::uint8_t, 20> digest{};
    if (mbedtls_sha1(data.data(), data.size(), digest.data()) != 0 || digest != chunk_sha) {
      return ChunkError::kChecksum;
    }
  }
  *out = std::move(data);
  return ChunkError::kNone;
}

ChunkError ProcessDepotChunkInPlace(std::vector<std::uint8_t>* encrypted,
                                   const std::array<std::uint8_t, 32>& key,
                                   std::size_t expected_size, std::uint32_t expected_checksum,
                                   const std::array<std::uint8_t, 20>& chunk_sha,
                                   std::vector<std::uint8_t>* out) {
  if (!encrypted || !out || encrypted->size() <= kAesBlock || encrypted->size() % kAesBlock != 0)
    return ChunkError::kDecrypt;
  out->clear();
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  std::uint8_t iv[kAesBlock];
  auto* payload = encrypted->data() + kAesBlock;
  const std::size_t payload_size = encrypted->size() - kAesBlock;
  const bool decrypted = mbedtls_aes_setkey_dec(&ctx, key.data(), 256) == 0 &&
      mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, encrypted->data(), iv) == 0 &&
      mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, payload_size, iv, payload, payload) == 0;
  mbedtls_aes_free(&ctx);
  if (!decrypted) return ChunkError::kDecrypt;
  const std::uint8_t pad = payload[payload_size - 1];
  if (pad == 0 || pad > kAesBlock || pad > payload_size) return ChunkError::kDecrypt;
  for (std::size_t i = payload_size - pad; i < payload_size; ++i)
    if (payload[i] != pad) return ChunkError::kDecrypt;
  const auto decoded = DecodeDepotChunk(payload, payload_size - pad, out);
  if (decoded != ChunkError::kNone) return decoded;
  if (out->size() != expected_size) return ChunkError::kSize;
  if (expected_checksum != 0) {
    if (DepotChunkAdler32(out->data(), out->size()) != expected_checksum) return ChunkError::kChecksum;
  } else {
    std::array<std::uint8_t, 20> digest{};
    if (mbedtls_sha1(out->data(), out->size(), digest.data()) != 0 || digest != chunk_sha)
      return ChunkError::kChecksum;
  }
  return ChunkError::kNone;
}

}  // namespace playden::steam
