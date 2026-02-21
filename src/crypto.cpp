#include "crypto.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace dlr {
namespace crypto {

static constexpr int IV_LEN  = 12;
static constexpr int TAG_LEN = 16;
static constexpr int KEY_LEN = 32; // AES-256

std::vector<uint8_t> generate_key() {
    std::vector<uint8_t> key(KEY_LEN);
    if (RAND_bytes(key.data(), KEY_LEN) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return key;
}

std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext,
                              const std::vector<uint8_t>& key) {
    if (key.size() != KEY_LEN) throw std::invalid_argument("Key must be 32 bytes");

    std::vector<uint8_t> iv(IV_LEN);
    if (RAND_bytes(iv.data(), IV_LEN) != 1) throw std::runtime_error("RAND_bytes failed");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<uint8_t> result;
    result.insert(result.end(), iv.begin(), iv.end()); // prepend IV

    std::vector<uint8_t> ciphertext(plaintext.size() + 16);
    int len = 0, total = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1 ||
        EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), (int)plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Encryption failed");
    }
    total = len;

    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + total, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EncryptFinal failed");
    }
    total += len;
    ciphertext.resize(total);

    std::vector<uint8_t> tag(TAG_LEN);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag.data());
    EVP_CIPHER_CTX_free(ctx);

    result.insert(result.end(), ciphertext.begin(), ciphertext.end());
    result.insert(result.end(), tag.begin(), tag.end());
    return result;
}

std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data,
                              const std::vector<uint8_t>& key) {
    if (key.size() != KEY_LEN) throw std::invalid_argument("Key must be 32 bytes");
    if (data.size() < IV_LEN + TAG_LEN) throw std::invalid_argument("Ciphertext too short");

    const uint8_t* iv         = data.data();
    const uint8_t* ciphertext = data.data() + IV_LEN;
    size_t ct_len             = data.size() - IV_LEN - TAG_LEN;
    const uint8_t* tag        = data.data() + IV_LEN + ct_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::vector<uint8_t> plaintext(ct_len + 16);
    int len = 0, total = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv) != 1 ||
        EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext, (int)ct_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Decryption failed");
    }
    total = len;
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, const_cast<uint8_t*>(tag));

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Authentication tag mismatch");
    }
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    plaintext.resize(total);
    return plaintext;
}

std::string sha256_hex(const std::string& data) {
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(data.data()), data.size(), hash);
    std::ostringstream ss;
    for (auto b : hash) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

std::string sha256_hex_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
        EVP_DigestUpdate(ctx, buf, f.gcount());
    uint8_t hash[SHA256_DIGEST_LENGTH];
    unsigned int len;
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);
    std::ostringstream ss;
    for (unsigned i = 0; i < len; i++) ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return ss.str();
}

static const char* B64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    int i = 0;
    uint8_t char3[3], char4[4];
    size_t in_len = data.size();
    const uint8_t* in = data.data();
    while (in_len--) {
        char3[i++] = *in++;
        if (i == 3) {
            char4[0] = (char3[0] & 0xfc) >> 2;
            char4[1] = ((char3[0] & 0x03) << 4) + ((char3[1] & 0xf0) >> 4);
            char4[2] = ((char3[1] & 0x0f) << 2) + ((char3[2] & 0xc0) >> 6);
            char4[3] = char3[2] & 0x3f;
            for (int j = 0; j < 4; j++) out += B64_TABLE[char4[j]];
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; j++) char3[j] = 0;
        char4[0] = (char3[0] & 0xfc) >> 2;
        char4[1] = ((char3[0] & 0x03) << 4) + ((char3[1] & 0xf0) >> 4);
        char4[2] = ((char3[1] & 0x0f) << 2) + ((char3[2] & 0xc0) >> 6);
        for (int j = 0; j < i + 1; j++) out += B64_TABLE[char4[j]];
        while (i++ < 3) out += '=';
    }
    return out;
}

std::vector<uint8_t> b64_decode(const std::string& encoded) {
    static const std::string chars = std::string(B64_TABLE);
    std::vector<uint8_t> out;
    int i = 0;
    uint8_t char3[3], char4[4];
    for (char c : encoded) {
        if (c == '=') break;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        char4[i++] = (uint8_t)pos;
        if (i == 4) {
            char3[0] = (char4[0] << 2) + ((char4[1] & 0x30) >> 4);
            char3[1] = ((char4[1] & 0xf) << 4) + ((char4[2] & 0x3c) >> 2);
            char3[2] = ((char4[2] & 0x3) << 6) + char4[3];
            for (int j = 0; j < 3; j++) out.push_back(char3[j]);
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 4; j++) char4[j] = 0;
        char3[0] = (char4[0] << 2) + ((char4[1] & 0x30) >> 4);
        char3[1] = ((char4[1] & 0xf) << 4) + ((char4[2] & 0x3c) >> 2);
        for (int j = 0; j < i - 1; j++) out.push_back(char3[j]);
    }
    return out;
}

} // namespace crypto
} // namespace dlr
