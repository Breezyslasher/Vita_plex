/**
 * VitaPlex - JWT Authentication utilities
 * Implements ED25519 signing for Plex JWT authentication
 */

#pragma once

#include <string>
#include <cstdint>

namespace vitaplex {

// ED25519 key sizes
constexpr size_t ED25519_PUBLIC_KEY_SIZE = 32;
constexpr size_t ED25519_PRIVATE_KEY_SIZE = 64;  // seed (32) + public (32)
constexpr size_t ED25519_SIGNATURE_SIZE = 64;
constexpr size_t ED25519_SEED_SIZE = 32;

/**
 * ED25519 Key Pair
 */
struct Ed25519KeyPair {
    uint8_t publicKey[ED25519_PUBLIC_KEY_SIZE];
    uint8_t privateKey[ED25519_PRIVATE_KEY_SIZE];
    std::string keyId;  // Key identifier for JWK

    bool isValid() const;
};

/**
 * JWT Authentication Manager
 * Handles ED25519 key generation, JWT signing, and Plex authentication
 */
class JwtAuth {
public:
    static JwtAuth& getInstance();

    /**
     * Initialize or load existing key pair
     * Keys are stored in ux0:data/VitaPlex/keys/
     */
    bool initialize();

    /**
     * Generate a new ED25519 key pair
     */
    bool generateKeyPair();

    /**
     * Load key pair from storage
     */
    bool loadKeyPair();

    /**
     * Save key pair to storage
     */
    bool saveKeyPair();

    /**
     * Get the JWK (JSON Web Key) representation of public key
     * Used for PIN registration with Plex
     */
    std::string getJwk() const;

    /**
     * Get the key ID
     */
    std::string getKeyId() const { return m_keyPair.keyId; }

    /**
     * Create a signed JWT for Plex authentication
     * @param nonce The nonce from Plex nonce endpoint
     * @param clientId The X-Plex-Client-Identifier
     * @param scope Comma-separated scopes (e.g., "username,email")
     */
    std::string createSignedJwt(const std::string& nonce,
                                const std::string& clientId,
                                const std::string& scope = "username,email");

    /**
     * Create a minimal JWT for PIN verification
     * @param clientId The X-Plex-Client-Identifier
     */
    std::string createPinVerificationJwt(const std::string& clientId);

    /**
     * Sign data using ED25519
     */
    bool sign(const uint8_t* message, size_t messageLen,
              uint8_t* signature) const;

    /**
     * Check if we have a valid key pair
     */
    bool hasValidKeyPair() const { return m_keyPair.isValid(); }

private:
    JwtAuth() = default;

    Ed25519KeyPair m_keyPair;
    bool m_initialized = false;

    // Helper functions
    std::string base64UrlEncode(const uint8_t* data, size_t len) const;
    std::string base64UrlEncode(const std::string& data) const;
    std::string generateKeyId() const;
    int64_t getCurrentTimestamp() const;
};

}  // namespace vitaplex
