#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dlr {
namespace crypto {

// AES-256-GCM encryption/decryption
// Returns encrypted bytes (12-byte nonce + ciphertext + 16-byte tag)
std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext,
                              const std::vector<uint8_t>& key);

std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext,
                              const std::vector<uint8_t>& key);

// Generate a random 256-bit session key
std::vector<uint8_t> generate_key();

// SHA-256 of data, returned as hex string
std::string sha256_hex(const std::string& data);
std::string sha256_hex_file(const std::string& path);

// Base64
std::string b64_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> b64_decode(const std::string& data);

} // namespace crypto
} // namespace dlr
