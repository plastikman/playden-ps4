/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_SHA256_H
#define PW_SHA256_H
#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4), for checking a download against its published
 * digest. Streaming: init, update as the bytes arrive, then final. */
typedef struct PwSha256 {
    uint32_t state[8];
    uint64_t length;                  /* bytes so far */
    uint8_t block[64];
    size_t used;
} PwSha256;

void pw_sha256_init(PwSha256 *sha);
void pw_sha256_update(PwSha256 *sha, const void *data, size_t size);
void pw_sha256_final(PwSha256 *sha, uint8_t digest[32]);
/* The digest as 64 lower-case hex characters and a NUL. */
void pw_sha256_hex(const uint8_t digest[32], char hex[65]);

#endif
