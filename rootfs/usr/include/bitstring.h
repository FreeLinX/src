#ifndef _BITSTRING_H_
#define _BITSTRING_H_

#include <limits.h>
#include <string.h>
#include <sys/types.h>

typedef unsigned char bitstr_t;

#define _BITSTR_MASK(bit)    ((unsigned char)1 << ((bit) % CHAR_BIT))

#define bit_decl(name, nbits) \
    (name)[((nbits) + CHAR_BIT - 1) / CHAR_BIT]

#define bit_clear(name, bit) \
    ((name)[(bit) / CHAR_BIT] &= ~_BITSTR_MASK(bit))

#define bit_set(name, bit) \
    ((name)[(bit) / CHAR_BIT] |= _BITSTR_MASK(bit))

#define bit_test(name, bit) \
    ((name)[(bit) / CHAR_BIT] & _BITSTR_MASK(bit))

#define bit_nclear(name, start, end) \
    do { \
        unsigned int _i; \
        for (_i = (start); _i <= (end); _i++) \
            bit_clear(name, _i); \
    } while (0)

#define bit_nset(name, start, end) \
    do { \
        unsigned int _i; \
        for (_i = (start); _i <= (end); _i++) \
            bit_set(name, _i); \
    } while (0)

#define bit_ffs(name, nbits, value) \
    do { \
        unsigned int _i; \
        *(value) = -1; \
        for (_i = 0; _i < (nbits); _i++) { \
            if (bit_test(name, _i)) { \
                *(value) = (int)_i; \
                break; \
            } \
        } \
    } while (0)

#define bit_ffc(name, nbits, value) \
    do { \
        unsigned int _i; \
        *(value) = -1; \
        for (_i = 0; _i < (nbits); _i++) { \
            if (!bit_test(name, _i)) { \
                *(value) = (int)_i; \
                break; \
            } \
        } \
    } while (0)

#endif /* _BITSTRING_H_ */
