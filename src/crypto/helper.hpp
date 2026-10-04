#ifndef CRYPTO_HELPER_HPP
#define CRYPTO_HELPER_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

std::vector<unsigned char> hex_to_bytes(const std::string& hex);

bool wallet_opening(
    const std::string& filepath,
    std::string* private_key_out = nullptr,
    std::string* public_key_out = nullptr,
    std::string* address_out = nullptr);

#endif