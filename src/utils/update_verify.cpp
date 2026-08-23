/**
 * VitaPlex - update artifact signature verification (see update_verify.hpp).
 */

#include "utils/update_verify.hpp"

// mbedcrypto is linked on every platform that installs updates in place:
// the consoles (Vita/PS4/Switch), Android and desktop Linux/macOS. Windows
// (Schannel, no mbedcrypto) and iOS/tvOS (browser-only updates) fall through
// to the stub at the bottom. CMake defines VITAPLEX_HAVE_MBEDCRYPTO for the
// former set.
#if defined(VITAPLEX_HAVE_MBEDCRYPTO)

#include <cstdio>
#include <cstring>

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#ifdef __PSV__
#include <psp2/io/fcntl.h>
#endif

namespace {

// ── The project's update-signing PUBLIC key ──────────────────────────────
// EC P-256, SPKI PEM. EMPTY = signature enforcement OFF, so this module ships
// inert and the updater behaves exactly as before. To ACTIVATE:
//   1. Generate a P-256 keypair (see docs/update-signing.md).
//   2. Keep the PRIVATE key ONLY as the CI secret UPDATE_SIGNING_KEY — never
//      commit it.
//   3. Paste the PUBLIC key (the whole PEM block, newline-terminated) here.
// From then on every in-app update is verified fail-closed on the platforms
// above.
const char kUpdatePublicKeyPem[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEOP4AMPm8n/jsPZJm80nt1ga8rFZe\n"
    "PppNk/oTCOJgZogO4wPu3gJM+rtYhHi5pptp757nQeaBbrNOFf0qDhH62A==\n"
    "-----END PUBLIC KEY-----\n";

// SHA-256 the file at `path` into `out`. Vita's newlib fopen is unreliable for
// the ux0: data paths the download writes to, so read via sceIo there (the
// same primitive the download used); every other target uses stdio.
bool sha256Path(const std::string& path, unsigned char out[32]) {
#ifdef __PSV__
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
#else
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
#endif

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_starts(&ctx, 0);
#else
    mbedtls_sha256_starts_ret(&ctx, 0);
#endif

    unsigned char buf[65536];
    bool ok = true;
    for (;;) {
#ifdef __PSV__
        int n = sceIoRead(fd, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        size_t got = (size_t)n;
#else
        size_t got = std::fread(buf, 1, sizeof(buf), f);
        if (got == 0) break;
#endif
#if MBEDTLS_VERSION_MAJOR >= 3
        mbedtls_sha256_update(&ctx, buf, got);
#else
        mbedtls_sha256_update_ret(&ctx, buf, got);
#endif
    }

#ifdef __PSV__
    sceIoClose(fd);
#else
    std::fclose(f);
#endif

#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_finish(&ctx, out);
#else
    mbedtls_sha256_finish_ret(&ctx, out);
#endif
    mbedtls_sha256_free(&ctx);
    return ok;
}

}  // namespace

namespace vitaplex {

bool updateSignatureEnforced() {
    return kUpdatePublicKeyPem[0] != '\0';
}

bool verifyUpdateFile(const std::string& filePath,
                      const std::string& signatureDer,
                      std::string& err) {
    if (kUpdatePublicKeyPem[0] == '\0') return true;   // inert: no key compiled in

    if (signatureDer.empty()) { err = "empty signature"; return false; }

    unsigned char hash[32];
    if (!sha256Path(filePath, hash)) { err = "cannot read downloaded file"; return false; }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    // The length passed to the PEM parser must include the terminating NUL,
    // which sizeof() on the char[] literal accounts for.
    int rc = mbedtls_pk_parse_public_key(
        &pk,
        reinterpret_cast<const unsigned char*>(kUpdatePublicKeyPem),
        sizeof(kUpdatePublicKeyPem));
    if (rc != 0) { err = "malformed update public key"; mbedtls_pk_free(&pk); return false; }

    // ECDSA verification is deterministic and needs no RNG.
    rc = mbedtls_pk_verify(
        &pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
        reinterpret_cast<const unsigned char*>(signatureDer.data()),
        signatureDer.size());
    mbedtls_pk_free(&pk);

    if (rc != 0) { err = "signature does not match this artifact"; return false; }
    return true;
}

}  // namespace vitaplex

#else  // no mbedcrypto: Windows (verified Schannel TLS) / iOS / tvOS (browser)

namespace vitaplex {

bool updateSignatureEnforced() { return false; }

bool verifyUpdateFile(const std::string&, const std::string&, std::string&) {
    return true;   // rely on TLS-verified transport / browser install flow
}

}  // namespace vitaplex

#endif  // VITAPLEX_HAVE_MBEDCRYPTO
