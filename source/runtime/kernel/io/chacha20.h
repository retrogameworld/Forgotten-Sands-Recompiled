#pragma once
// Step 27: ChaCha20 (RFC 8439) for the game pak. Seekable: XOR-s data that starts at byte `pos` of the
// key stream (block counter = pos / 64), so any part of a file can be decrypted on its own.
#include <cstdint>
#include <cstring>

namespace ChaCha20
{
    inline uint32_t Rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

    inline void Block(const uint32_t key[8], const uint32_t nonce[3], uint32_t counter, uint8_t out[64])
    {
        uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
            key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], counter, nonce[0], nonce[1], nonce[2] };
        uint32_t x[16];
        memcpy(x, s, sizeof(x));
        auto qr = [&](int a, int b, int c, int d)
        {
            x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 16);
            x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 12);
            x[a] += x[b]; x[d] = Rotl(x[d] ^ x[a], 8);
            x[c] += x[d]; x[b] = Rotl(x[b] ^ x[c], 7);
        };
        for (int i = 0; i < 10; i++)
        {
            qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
            qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
        }
        for (int i = 0; i < 16; i++)
        {
            const uint32_t v = x[i] + s[i];
            out[i * 4 + 0] = uint8_t(v); out[i * 4 + 1] = uint8_t(v >> 8); out[i * 4 + 2] = uint8_t(v >> 16); out[i * 4 + 3] = uint8_t(v >> 24);
        }
    }

    // data[0..n) ^= keystream[pos..pos+n). The 64-bit position also feeds the nonce's first word, so files
    // larger than 256 GB would still work.
    inline void Xor(const uint32_t key[8], const uint32_t nonceIn[3], uint64_t pos, uint8_t* data, size_t n)
    {
        uint8_t ks[64];
        while (n)
        {
            const uint64_t block = pos / 64;
            const size_t offset = size_t(pos % 64);
            const uint32_t nonce[3] = { nonceIn[0] ^ uint32_t(block >> 32), nonceIn[1], nonceIn[2] };
            Block(key, nonce, uint32_t(block), ks);
            const size_t take = n < 64 - offset ? n : 64 - offset;
            for (size_t i = 0; i < take; i++)
                data[i] ^= ks[offset + i];
            data += take;
            n -= take;
            pos += take;
        }
    }
}
