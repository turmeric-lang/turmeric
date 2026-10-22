/* sha256.h -- streaming SHA-256.
 *
 * One implementation for the whole tree.  It exists because two callers need
 * a digest and neither can shell out for one:
 *
 *   - src/runtime/image.c   `tur image-verify` stamps a build against a binary.
 *   - src/compiler/pkg.c    the spice lockfile's integrity check.
 *
 * pkg.c used to run `shasum -a 256` / `sha256sum` through popen with POSIX
 * shell quoting, which no Windows host can satisfy: MinGW has neither tool and
 * popen there runs cmd.exe, where `'...'` is not quoting at all.  See
 * docs/archive/pkg-hash-shells-out-to-sha256sum.md. */
#ifndef TUR_SHA256_H
#define TUR_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUR_SHA256_DIGEST_LEN 32
#define TUR_SHA256_HEX_LEN    64   /* not counting the NUL */

typedef struct {
    uint32_t state[8];
    uint64_t nbytes;              /* total fed in, for the length pad */
    uint8_t  block[64];
    size_t   blocklen;            /* bytes buffered in `block` */
} TurSha256;

void tur_sha256_init(TurSha256 *ctx);
void tur_sha256_update(TurSha256 *ctx, const void *data, size_t len);
void tur_sha256_final(TurSha256 *ctx, uint8_t out[TUR_SHA256_DIGEST_LEN]);

/* Lowercase hex, NUL-terminated: 65 bytes written. */
void tur_sha256_hex(const uint8_t digest[TUR_SHA256_DIGEST_LEN], char out[65]);

/* Digest a file's bytes, streaming.  false if it cannot be read. */
bool tur_sha256_file(const char *path, uint8_t out[TUR_SHA256_DIGEST_LEN]);

#endif /* TUR_SHA256_H */
