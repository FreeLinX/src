#ifndef FLX_SHA1_H
#define FLX_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_LENGTH 20
#define SHA1_DIGEST_STRING_LENGTH 41

typedef struct {
    uint32_t state[5];
    uint64_t count;
    unsigned char buffer[64];
} SHA1_CTX;

void SHA1Init(SHA1_CTX *context);
void SHA1Update(SHA1_CTX *context, const unsigned char *data, size_t len);
void SHA1Final(unsigned char digest[SHA1_DIGEST_LENGTH], SHA1_CTX *context);

#endif