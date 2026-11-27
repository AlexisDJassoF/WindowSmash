/* SHA-256 (FIPS 180-4), for the room's mods (netplay-mod-sync): each file is known by its hash. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t used; } Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);
/* A file's: 1 read (*size its bytes), 0 not */
int sha256_file(const char *path, uint8_t out[32], uint64_t *size);

void sha256_hex(const uint8_t h[32], char out[65]);
int sha256_parse(const char *hex, uint8_t out[32]);   /* 64 hex digits: 1 */
uint64_t sha256_u64(const uint8_t h[32]);             /* its first 8 bytes, big-endian: the short form packets carry */
