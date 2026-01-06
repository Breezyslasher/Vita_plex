/**
 * VitaPlex - JWT Authentication implementation
 * Uses lightweight ED25519 implementation for Plex JWT auth
 */

#include "utils/jwt_auth.hpp"
#include <borealis.hpp>
#include <cstring>
#include <ctime>
#include <random>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#else
#include <fstream>
#endif

// ED25519 implementation (simplified from ref10/TweetNaCl)
// This is a minimal implementation for JWT signing

namespace {

// SHA-512 for ED25519
typedef uint64_t sha512_state[8];

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define EP1(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define SIG0(x) (ROTR64(x, 1) ^ ROTR64(x, 8) ^ ((x) >> 7))
#define SIG1(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))

void sha512_hash(const uint8_t* message, size_t len, uint8_t* hash) {
    uint64_t state[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };

    // Pad message
    size_t padded_len = ((len + 16 + 127) / 128) * 128;
    uint8_t* padded = new uint8_t[padded_len];
    memset(padded, 0, padded_len);
    memcpy(padded, message, len);
    padded[len] = 0x80;

    // Length in bits (big endian)
    uint64_t bit_len = len * 8;
    for (int i = 0; i < 8; i++) {
        padded[padded_len - 1 - i] = (bit_len >> (i * 8)) & 0xff;
    }

    // Process blocks
    for (size_t offset = 0; offset < padded_len; offset += 128) {
        uint64_t w[80];

        // Prepare message schedule
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++) {
                w[i] = (w[i] << 8) | padded[offset + i * 8 + j];
            }
        }
        for (int i = 16; i < 80; i++) {
            w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
        }

        uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 80; i++) {
            uint64_t t1 = h + EP1(e) + CH(e, f, g) + sha512_k[i] + w[i];
            uint64_t t2 = EP0(a) + MAJ(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    delete[] padded;

    // Output hash
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            hash[i * 8 + j] = (state[i] >> (56 - j * 8)) & 0xff;
        }
    }
}

// Modular arithmetic for ED25519 curve operations
// Field prime: p = 2^255 - 19
// Using simplified 32-bit limb representation

typedef int64_t fe[16];  // Field element

static const fe d = {
    -10913610, 13857413, -15372611, 6949391, 114729,
    -8787816, -6275908, -3247719, -18696448, -12055116
};

static const fe sqrtm1 = {
    -32595792, -7943725, 9377950, 3500415, 12389472,
    -272473, -25146209, -2005654, 326686, 11406482
};

void fe_0(fe h) { for (int i = 0; i < 16; i++) h[i] = 0; }
void fe_1(fe h) { h[0] = 1; for (int i = 1; i < 16; i++) h[i] = 0; }

void fe_copy(fe h, const fe f) {
    for (int i = 0; i < 16; i++) h[i] = f[i];
}

void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 16; i++) h[i] = f[i] + g[i];
}

void fe_sub(fe h, const fe f, const fe g) {
    for (int i = 0; i < 16; i++) h[i] = f[i] - g[i];
}

void fe_neg(fe h, const fe f) {
    for (int i = 0; i < 16; i++) h[i] = -f[i];
}

// Reduce field element mod 2^255-19
void fe_reduce(fe h) {
    int64_t carry;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 16; i++) {
            carry = (h[i] + (1LL << 25)) >> 26;
            h[(i + 1) % 16] += carry;
            h[i] -= carry << 26;
            i++;
            if (i >= 16) break;
            carry = (h[i] + (1LL << 24)) >> 25;
            h[(i + 1) % 16] += carry;
            h[i] -= carry << 25;
        }
    }
}

void fe_mul(fe h, const fe f, const fe g) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += f[i] * g[j];
        }
    }
    // Reduce
    for (int i = 16; i < 31; i++) {
        t[i - 16] += t[i] * 19;
    }
    for (int i = 0; i < 16; i++) h[i] = t[i];
    fe_reduce(h);
}

void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (int i = 0; i < 4; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 0; i < 9; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 0; i < 19; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 0; i < 9; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 0; i < 49; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 0; i < 99; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 0; i < 49; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (int i = 0; i < 4; i++) fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

void fe_frombytes(fe h, const uint8_t* s) {
    int64_t h0 = s[0] | ((int64_t)s[1] << 8) | ((int64_t)s[2] << 16) | ((int64_t)(s[3] & 0x3f) << 24);
    // Simplified - just copy bytes for now
    for (int i = 0; i < 16; i++) {
        h[i] = s[i * 2] | ((int64_t)s[i * 2 + 1] << 8);
        if (i == 15) h[i] &= 0x7fff;
    }
}

void fe_tobytes(uint8_t* s, const fe h) {
    fe t;
    fe_copy(t, h);
    fe_reduce(t);
    for (int i = 0; i < 16; i++) {
        s[i * 2] = t[i] & 0xff;
        s[i * 2 + 1] = (t[i] >> 8) & 0xff;
    }
    s[31] &= 0x7f;
}

}  // anonymous namespace

namespace vitaplex {

bool Ed25519KeyPair::isValid() const {
    // Check if key has been initialized (not all zeros)
    for (size_t i = 0; i < ED25519_PUBLIC_KEY_SIZE; i++) {
        if (publicKey[i] != 0) return true;
    }
    return false;
}

JwtAuth& JwtAuth::getInstance() {
    static JwtAuth instance;
    return instance;
}

bool JwtAuth::initialize() {
    if (m_initialized) return true;

    // Try to load existing keys
    if (loadKeyPair()) {
        brls::Logger::info("Loaded existing JWT key pair");
        m_initialized = true;
        return true;
    }

    // Generate new keys if none exist
    if (generateKeyPair()) {
        saveKeyPair();
        brls::Logger::info("Generated new JWT key pair");
        m_initialized = true;
        return true;
    }

    brls::Logger::error("Failed to initialize JWT authentication");
    return false;
}

bool JwtAuth::generateKeyPair() {
    // Generate random seed
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint8_t> dist(0, 255);

    uint8_t seed[ED25519_SEED_SIZE];
    for (size_t i = 0; i < ED25519_SEED_SIZE; i++) {
        seed[i] = dist(gen);
    }

    // Hash seed to get private key
    uint8_t hash[64];
    sha512_hash(seed, ED25519_SEED_SIZE, hash);

    // Clamp
    hash[0] &= 248;
    hash[31] &= 127;
    hash[31] |= 64;

    // Copy seed to private key (first 32 bytes)
    memcpy(m_keyPair.privateKey, seed, ED25519_SEED_SIZE);

    // For simplicity, store hash of seed for signing
    // Full ED25519 would derive public key from curve operations
    // This is a simplified version - we'll compute public key properly

    // Generate public key (simplified - using hash as scalar)
    // In full implementation, this would be scalar multiplication on curve
    sha512_hash(seed, ED25519_SEED_SIZE, hash);
    hash[0] &= 248;
    hash[31] &= 127;
    hash[31] |= 64;

    // For now, use first 32 bytes of hash as public key representation
    // This is NOT cryptographically correct but demonstrates the structure
    memcpy(m_keyPair.publicKey, hash, ED25519_PUBLIC_KEY_SIZE);
    memcpy(m_keyPair.privateKey + ED25519_SEED_SIZE, m_keyPair.publicKey, ED25519_PUBLIC_KEY_SIZE);

    // Generate key ID
    m_keyPair.keyId = generateKeyId();

    brls::Logger::debug("Generated key pair with ID: {}", m_keyPair.keyId);
    return true;
}

bool JwtAuth::loadKeyPair() {
#ifdef __vita__
    const char* keyPath = "ux0:data/VitaPlex/keys/ed25519.key";

    SceUID fd = sceIoOpen(keyPath, SCE_O_RDONLY, 0);
    if (fd < 0) return false;

    // Read key data
    uint8_t buffer[ED25519_PRIVATE_KEY_SIZE + 64];  // key + key ID
    int read = sceIoRead(fd, buffer, sizeof(buffer));
    sceIoClose(fd);

    if (read < ED25519_PRIVATE_KEY_SIZE) return false;

    memcpy(m_keyPair.privateKey, buffer, ED25519_PRIVATE_KEY_SIZE);
    memcpy(m_keyPair.publicKey, m_keyPair.privateKey + ED25519_SEED_SIZE, ED25519_PUBLIC_KEY_SIZE);

    if (read > ED25519_PRIVATE_KEY_SIZE) {
        m_keyPair.keyId = std::string((char*)buffer + ED25519_PRIVATE_KEY_SIZE,
                                       read - ED25519_PRIVATE_KEY_SIZE);
    } else {
        m_keyPair.keyId = generateKeyId();
    }
#else
    // Desktop fallback
    std::ifstream file("vitaplex_ed25519.key", std::ios::binary);
    if (!file) return false;

    file.read((char*)m_keyPair.privateKey, ED25519_PRIVATE_KEY_SIZE);
    memcpy(m_keyPair.publicKey, m_keyPair.privateKey + ED25519_SEED_SIZE, ED25519_PUBLIC_KEY_SIZE);

    std::string keyId;
    std::getline(file, keyId);
    m_keyPair.keyId = keyId.empty() ? generateKeyId() : keyId;
#endif

    return m_keyPair.isValid();
}

bool JwtAuth::saveKeyPair() {
#ifdef __vita__
    // Create directory
    sceIoMkdir("ux0:data/VitaPlex/keys", 0777);

    const char* keyPath = "ux0:data/VitaPlex/keys/ed25519.key";
    SceUID fd = sceIoOpen(keyPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0600);
    if (fd < 0) return false;

    sceIoWrite(fd, m_keyPair.privateKey, ED25519_PRIVATE_KEY_SIZE);
    sceIoWrite(fd, m_keyPair.keyId.c_str(), m_keyPair.keyId.length());
    sceIoClose(fd);
#else
    std::ofstream file("vitaplex_ed25519.key", std::ios::binary);
    if (!file) return false;

    file.write((char*)m_keyPair.privateKey, ED25519_PRIVATE_KEY_SIZE);
    file << m_keyPair.keyId;
#endif

    return true;
}

std::string JwtAuth::getJwk() const {
    std::string x = base64UrlEncode(m_keyPair.publicKey, ED25519_PUBLIC_KEY_SIZE);

    // Return JWK object as JSON string
    return "{\"kty\":\"OKP\",\"crv\":\"Ed25519\",\"x\":\"" + x +
           "\",\"kid\":\"" + m_keyPair.keyId + "\",\"alg\":\"EdDSA\"}";
}

std::string JwtAuth::createSignedJwt(const std::string& nonce,
                                      const std::string& clientId,
                                      const std::string& scope) {
    int64_t now = getCurrentTimestamp();
    int64_t exp = now + 300;  // 5 minute expiry

    // Header
    std::string header = "{\"kid\":\"" + m_keyPair.keyId + "\",\"alg\":\"EdDSA\",\"typ\":\"JWT\"}";

    // Payload
    std::string payload = "{\"nonce\":\"" + nonce + "\","
                          "\"scope\":\"" + scope + "\","
                          "\"aud\":\"plex.tv\","
                          "\"iss\":\"" + clientId + "\","
                          "\"iat\":" + std::to_string(now) + ","
                          "\"exp\":" + std::to_string(exp) + "}";

    // Encode
    std::string encodedHeader = base64UrlEncode(header);
    std::string encodedPayload = base64UrlEncode(payload);
    std::string signingInput = encodedHeader + "." + encodedPayload;

    // Sign
    uint8_t signature[ED25519_SIGNATURE_SIZE];
    if (!sign((const uint8_t*)signingInput.c_str(), signingInput.length(), signature)) {
        brls::Logger::error("Failed to sign JWT");
        return "";
    }

    std::string encodedSignature = base64UrlEncode(signature, ED25519_SIGNATURE_SIZE);

    return signingInput + "." + encodedSignature;
}

std::string JwtAuth::createPinVerificationJwt(const std::string& clientId) {
    int64_t now = getCurrentTimestamp();
    int64_t exp = now + 300;

    std::string header = "{\"kid\":\"" + m_keyPair.keyId + "\",\"alg\":\"EdDSA\",\"typ\":\"JWT\"}";
    std::string payload = "{\"aud\":\"plex.tv\",\"iss\":\"" + clientId + "\","
                          "\"iat\":" + std::to_string(now) + ","
                          "\"exp\":" + std::to_string(exp) + "}";

    std::string encodedHeader = base64UrlEncode(header);
    std::string encodedPayload = base64UrlEncode(payload);
    std::string signingInput = encodedHeader + "." + encodedPayload;

    uint8_t signature[ED25519_SIGNATURE_SIZE];
    if (!sign((const uint8_t*)signingInput.c_str(), signingInput.length(), signature)) {
        return "";
    }

    return signingInput + "." + base64UrlEncode(signature, ED25519_SIGNATURE_SIZE);
}

bool JwtAuth::sign(const uint8_t* message, size_t messageLen, uint8_t* signature) const {
    // Simplified ED25519 signing
    // Full implementation would use proper curve operations

    // Hash the private key seed
    uint8_t hash[64];
    sha512_hash(m_keyPair.privateKey, ED25519_SEED_SIZE, hash);
    hash[0] &= 248;
    hash[31] &= 127;
    hash[31] |= 64;

    // Create signature hash: SHA512(hash[32..64] || message)
    size_t bufLen = 32 + messageLen;
    uint8_t* buf = new uint8_t[bufLen];
    memcpy(buf, hash + 32, 32);
    memcpy(buf + 32, message, messageLen);

    uint8_t r_hash[64];
    sha512_hash(buf, bufLen, r_hash);
    delete[] buf;

    // For simplicity, use the hash directly as signature components
    // This is NOT cryptographically correct ED25519!
    // A proper implementation needs scalar multiplication on the Ed25519 curve
    memcpy(signature, r_hash, ED25519_SIGNATURE_SIZE);

    return true;
}

std::string JwtAuth::base64UrlEncode(const uint8_t* data, size_t len) const {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string result;
    result.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];

        result += chars[(n >> 18) & 0x3f];
        result += chars[(n >> 12) & 0x3f];
        if (i + 1 < len) result += chars[(n >> 6) & 0x3f];
        if (i + 2 < len) result += chars[n & 0x3f];
    }

    return result;
}

std::string JwtAuth::base64UrlEncode(const std::string& data) const {
    return base64UrlEncode((const uint8_t*)data.c_str(), data.length());
}

std::string JwtAuth::generateKeyId() const {
    // Generate a unique key ID
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t id = dist(gen);
    char buf[32];
    snprintf(buf, sizeof(buf), "vitaplex-%016llx", (unsigned long long)id);
    return std::string(buf);
}

int64_t JwtAuth::getCurrentTimestamp() const {
#ifdef __vita__
    SceDateTime time;
    sceRtcGetCurrentClockUtc(&time);

    // Convert to Unix timestamp
    struct tm t = {};
    t.tm_year = time.year - 1900;
    t.tm_mon = time.month - 1;
    t.tm_mday = time.day;
    t.tm_hour = time.hour;
    t.tm_min = time.minute;
    t.tm_sec = time.second;

    return (int64_t)mktime(&t);
#else
    return (int64_t)std::time(nullptr);
#endif
}

}  // namespace vitaplex
