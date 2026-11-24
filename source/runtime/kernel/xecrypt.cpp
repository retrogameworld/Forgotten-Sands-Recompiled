#include <stdafx.h>
#include "function.h"
#include "import_log.h"
#include "memory.h"

// XeCryptSha* (SHA-1, SHA-256, SHA-384, SHA-512) with the console's state layouts.
// Plain portable implementations (FIPS 180-4), no external crypto library.

namespace
{
    inline uint32_t Rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    inline uint32_t Ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    inline uint64_t Ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
    inline uint32_t LoadBE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    inline uint64_t LoadBE64(const uint8_t* p) { return (uint64_t(LoadBE32(p)) << 32) | LoadBE32(p + 4); }

    struct XECRYPT_SHA_STATE
    {
        be<uint32_t> count;
        be<uint32_t> state[5];
        uint8_t buffer[64];
    };
    static_assert(sizeof(XECRYPT_SHA_STATE) == 0x58);

    struct XECRYPT_SHA256_STATE
    {
        be<uint32_t> count;
        be<uint32_t> state[8];
        uint8_t buffer[64];
    };
    static_assert(sizeof(XECRYPT_SHA256_STATE) == 0x64);

    struct XECRYPT_SHA512_STATE
    {
        be<uint64_t> count;
        be<uint64_t> state[8];
        uint8_t buffer[128];
    };
    static_assert(sizeof(XECRYPT_SHA512_STATE) == 0xC8);

    void Sha1Block(be<uint32_t>* st, const uint8_t* block)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = LoadBE32(block + i * 4);
        for (int i = 16; i < 80; i++) w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = Rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = Rol32(b, 30); b = a; a = t;
        }
        st[0] = st[0] + a; st[1] = st[1] + b; st[2] = st[2] + c; st[3] = st[3] + d; st[4] = st[4] + e;
    }

    const uint32_t K256[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

    void Sha256Block(be<uint32_t>* st, const uint8_t* block)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = LoadBE32(block + i * 4);
        for (int i = 16; i < 64; i++)
        {
            uint32_t s0 = Ror32(w[i - 15], 7) ^ Ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Ror32(w[i - 2], 17) ^ Ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        for (int i = 0; i < 8; i++) v[i] = st[i];
        for (int i = 0; i < 64; i++)
        {
            uint32_t S1 = Ror32(v[4], 6) ^ Ror32(v[4], 11) ^ Ror32(v[4], 25);
            uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            uint32_t t1 = v[7] + S1 + ch + K256[i] + w[i];
            uint32_t S0 = Ror32(v[0], 2) ^ Ror32(v[0], 13) ^ Ror32(v[0], 22);
            uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            uint32_t t2 = S0 + mj;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) st[i] = st[i] + v[i];
    }

    const uint64_t K512[80] = {
        0x428a2f98d728ae22ull,0x7137449123ef65cdull,0xb5c0fbcfec4d3b2full,0xe9b5dba58189dbbcull,0x3956c25bf348b538ull,
        0x59f111f1b605d019ull,0x923f82a4af194f9bull,0xab1c5ed5da6d8118ull,0xd807aa98a3030242ull,0x12835b0145706fbeull,
        0x243185be4ee4b28cull,0x550c7dc3d5ffb4e2ull,0x72be5d74f27b896full,0x80deb1fe3b1696b1ull,0x9bdc06a725c71235ull,
        0xc19bf174cf692694ull,0xe49b69c19ef14ad2ull,0xefbe4786384f25e3ull,0x0fc19dc68b8cd5b5ull,0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull,0x4a7484aa6ea6e483ull,0x5cb0a9dcbd41fbd4ull,0x76f988da831153b5ull,0x983e5152ee66dfabull,
        0xa831c66d2db43210ull,0xb00327c898fb213full,0xbf597fc7beef0ee4ull,0xc6e00bf33da88fc2ull,0xd5a79147930aa725ull,
        0x06ca6351e003826full,0x142929670a0e6e70ull,0x27b70a8546d22ffcull,0x2e1b21385c26c926ull,0x4d2c6dfc5ac42aedull,
        0x53380d139d95b3dfull,0x650a73548baf63deull,0x766a0abb3c77b2a8ull,0x81c2c92e47edaee6ull,0x92722c851482353bull,
        0xa2bfe8a14cf10364ull,0xa81a664bbc423001ull,0xc24b8b70d0f89791ull,0xc76c51a30654be30ull,0xd192e819d6ef5218ull,
        0xd69906245565a910ull,0xf40e35855771202aull,0x106aa07032bbd1b8ull,0x19a4c116b8d2d0c8ull,0x1e376c085141ab53ull,
        0x2748774cdf8eeb99ull,0x34b0bcb5e19b48a8ull,0x391c0cb3c5c95a63ull,0x4ed8aa4ae3418acbull,0x5b9cca4f7763e373ull,
        0x682e6ff3d6b2b8a3ull,0x748f82ee5defb2fcull,0x78a5636f43172f60ull,0x84c87814a1f0ab72ull,0x8cc702081a6439ecull,
        0x90befffa23631e28ull,0xa4506cebde82bde9ull,0xbef9a3f7b2c67915ull,0xc67178f2e372532bull,0xca273eceea26619cull,
        0xd186b8c721c0c207ull,0xeada7dd6cde0eb1eull,0xf57d4f7fee6ed178ull,0x06f067aa72176fbaull,0x0a637dc5a2c898a6ull,
        0x113f9804bef90daeull,0x1b710b35131c471bull,0x28db77f523047d84ull,0x32caab7b40c72493ull,0x3c9ebe0a15c9bebcull,
        0x431d67c49c100d4cull,0x4cc5d4becb3e42b6ull,0x597f299cfc657e2aull,0x5fcb6fab3ad6faecull,0x6c44198c4a475817ull };

    void Sha512Block(be<uint64_t>* st, const uint8_t* block)
    {
        uint64_t w[80];
        for (int i = 0; i < 16; i++) w[i] = LoadBE64(block + i * 8);
        for (int i = 16; i < 80; i++)
        {
            uint64_t s0 = Ror64(w[i - 15], 1) ^ Ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = Ror64(w[i - 2], 19) ^ Ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t v[8];
        for (int i = 0; i < 8; i++) v[i] = st[i];
        for (int i = 0; i < 80; i++)
        {
            uint64_t S1 = Ror64(v[4], 14) ^ Ror64(v[4], 18) ^ Ror64(v[4], 41);
            uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            uint64_t t1 = v[7] + S1 + ch + K512[i] + w[i];
            uint64_t S0 = Ror64(v[0], 28) ^ Ror64(v[0], 34) ^ Ror64(v[0], 39);
            uint64_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            uint64_t t2 = S0 + mj;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) st[i] = st[i] + v[i];
    }

    // Generic Merkle-Damgard update/final over a block function.
    template<typename TCount, size_t BlockSize, typename TBlockFunc>
    void Update(be<TCount>& count, uint8_t* buffer, TBlockFunc&& blockFunc, const uint8_t* data, size_t size)
    {
        uint64_t total = count;
        size_t used = size_t(total % BlockSize);
        count = TCount(total + size);

        while (size > 0)
        {
            size_t n = std::min(BlockSize - used, size);
            memcpy(buffer + used, data, n);
            used += n; data += n; size -= n;
            if (used == BlockSize)
            {
                blockFunc(buffer);
                used = 0;
            }
        }
    }

    template<typename TCount, size_t BlockSize, size_t LengthBytes, typename TBlockFunc>
    void Final(be<TCount>& count, uint8_t* buffer, TBlockFunc&& blockFunc)
    {
        uint64_t totalBits = uint64_t(count.get()) * 8;
        size_t used = size_t(uint64_t(count.get()) % BlockSize);
        buffer[used++] = 0x80;
        if (used > BlockSize - LengthBytes)
        {
            memset(buffer + used, 0, BlockSize - used);
            blockFunc(buffer);
            used = 0;
        }
        memset(buffer + used, 0, BlockSize - used);
        for (int i = 0; i < 8; i++)
            buffer[BlockSize - 1 - i] = uint8_t(totalBits >> (i * 8));
        blockFunc(buffer);
    }
}

void XeCryptShaInit(XECRYPT_SHA_STATE* s)
{
    memset(s, 0, sizeof(*s));
    s->state[0] = 0x67452301; s->state[1] = 0xEFCDAB89; s->state[2] = 0x98BADCFE; s->state[3] = 0x10325476; s->state[4] = 0xC3D2E1F0;
}

void XeCryptShaUpdate(XECRYPT_SHA_STATE* s, const uint8_t* data, uint32_t size)
{
    Update<uint32_t, 64>(s->count, s->buffer, [s](const uint8_t* b) { Sha1Block(s->state, b); }, data, size);
}

void XeCryptShaFinal(XECRYPT_SHA_STATE* s, uint8_t* out, uint32_t outSize)
{
    Final<uint32_t, 64, 8>(s->count, s->buffer, [s](const uint8_t* b) { Sha1Block(s->state, b); });
    uint8_t digest[20];
    for (int i = 0; i < 5; i++) *(uint32_t*)(digest + i * 4) = ByteSwap(s->state[i].get());
    memcpy(out, digest, std::min<uint32_t>(outSize, 20));
}

static void Sha256InitState(XECRYPT_SHA256_STATE* s, const uint32_t (&iv)[8])
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++) s->state[i] = iv[i];
}

void XeCryptSha256Init(XECRYPT_SHA256_STATE* s)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    Sha256InitState(s, iv);
}

void XeCryptSha256Update(XECRYPT_SHA256_STATE* s, const uint8_t* data, uint32_t size)
{
    Update<uint32_t, 64>(s->count, s->buffer, [s](const uint8_t* b) { Sha256Block(s->state, b); }, data, size);
}

void XeCryptSha256Final(XECRYPT_SHA256_STATE* s, uint8_t* out, uint32_t outSize)
{
    Final<uint32_t, 64, 8>(s->count, s->buffer, [s](const uint8_t* b) { Sha256Block(s->state, b); });
    uint8_t digest[32];
    for (int i = 0; i < 8; i++) *(uint32_t*)(digest + i * 4) = ByteSwap(s->state[i].get());
    memcpy(out, digest, std::min<uint32_t>(outSize, 32));
}

static void Sha512InitState(XECRYPT_SHA512_STATE* s, const uint64_t (&iv)[8])
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++) s->state[i] = iv[i];
}

void XeCryptSha384Init(XECRYPT_SHA512_STATE* s)
{
    static const uint64_t iv[8] = { 0xcbbb9d5dc1059ed8ull,0x629a292a367cd507ull,0x9159015a3070dd17ull,0x152fecd8f70e5939ull,
        0x67332667ffc00b31ull,0x8eb44a8768581511ull,0xdb0c2e0d64f98fa7ull,0x47b5481dbefa4fa4ull };
    Sha512InitState(s, iv);
}

void XeCryptSha512Init(XECRYPT_SHA512_STATE* s)
{
    static const uint64_t iv[8] = { 0x6a09e667f3bcc908ull,0xbb67ae8584caa73bull,0x3c6ef372fe94f82bull,0xa54ff53a5f1d36f1ull,
        0x510e527fade682d1ull,0x9b05688c2b3e6c1full,0x1f83d9abfb41bd6bull,0x5be0cd19137e2179ull };
    Sha512InitState(s, iv);
}

void XeCryptSha512Update(XECRYPT_SHA512_STATE* s, const uint8_t* data, uint32_t size)
{
    Update<uint64_t, 128>(s->count, s->buffer, [s](const uint8_t* b) { Sha512Block(s->state, b); }, data, size);
}

static void Sha512FinalN(XECRYPT_SHA512_STATE* s, uint8_t* out, uint32_t outSize, uint32_t digestSize)
{
    Final<uint64_t, 128, 16>(s->count, s->buffer, [s](const uint8_t* b) { Sha512Block(s->state, b); });
    uint8_t digest[64];
    for (int i = 0; i < 8; i++) *(uint64_t*)(digest + i * 8) = ByteSwap(s->state[i].get());
    memcpy(out, digest, std::min<uint32_t>(outSize, digestSize));
}

void XeCryptSha384Update(XECRYPT_SHA512_STATE* s, const uint8_t* data, uint32_t size) { XeCryptSha512Update(s, data, size); }
void XeCryptSha384Final(XECRYPT_SHA512_STATE* s, uint8_t* out, uint32_t outSize) { Sha512FinalN(s, out, outSize, 48); }
void XeCryptSha512Final(XECRYPT_SHA512_STATE* s, uint8_t* out, uint32_t outSize) { Sha512FinalN(s, out, outSize, 64); }

// Self test at startup (called from main): SHA of "abc".
bool XeCryptSelfTest()
{
    const uint8_t abc[3] = { 'a', 'b', 'c' };
    uint8_t out[64];

    XECRYPT_SHA_STATE s1; XeCryptShaInit(&s1); XeCryptShaUpdate(&s1, abc, 3); XeCryptShaFinal(&s1, out, 20);
    bool ok = out[0] == 0xA9 && out[1] == 0x99 && out[19] == 0x9D;

    XECRYPT_SHA256_STATE s2; XeCryptSha256Init(&s2); XeCryptSha256Update(&s2, abc, 3); XeCryptSha256Final(&s2, out, 32);
    ok = ok && out[0] == 0xBA && out[1] == 0x78 && out[31] == 0xAD;

    XECRYPT_SHA512_STATE s3; XeCryptSha512Init(&s3); XeCryptSha512Update(&s3, abc, 3); XeCryptSha512Final(&s3, out, 64);
    ok = ok && out[0] == 0xDD && out[1] == 0xAF && out[63] == 0x9F;

    XeCryptSha384Init(&s3); XeCryptSha384Update(&s3, abc, 3); XeCryptSha384Final(&s3, out, 48);
    ok = ok && out[0] == 0xCB && out[1] == 0x00 && out[47] == 0xA7;
    return ok;
}

GUEST_IMPORT(XeCryptShaInit, XeCryptShaInit);
GUEST_IMPORT(XeCryptShaUpdate, XeCryptShaUpdate);
GUEST_IMPORT(XeCryptShaFinal, XeCryptShaFinal);
GUEST_IMPORT(XeCryptSha256Init, XeCryptSha256Init);
GUEST_IMPORT(XeCryptSha256Update, XeCryptSha256Update);
GUEST_IMPORT(XeCryptSha256Final, XeCryptSha256Final);
GUEST_IMPORT(XeCryptSha384Init, XeCryptSha384Init);
GUEST_IMPORT(XeCryptSha384Update, XeCryptSha384Update);
GUEST_IMPORT(XeCryptSha384Final, XeCryptSha384Final);
GUEST_IMPORT(XeCryptSha512Init, XeCryptSha512Init);
GUEST_IMPORT(XeCryptSha512Update, XeCryptSha512Update);
GUEST_IMPORT(XeCryptSha512Final, XeCryptSha512Final);
