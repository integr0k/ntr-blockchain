#include "helper.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <vector>

std::vector<unsigned char> hex_to_bytes(const std::string& hex) {
    std::vector<unsigned char> bytes;

    if (hex.length() % 2 != 0) {
        std::cerr << "Error: HEX string must have an even length!" << std::endl;
        return bytes;
    }

    for (std::size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_string = hex.substr(i, 2);
        unsigned char byte = static_cast<unsigned char>(std::stoul(byte_string, nullptr, 16));
        bytes.push_back(byte);
    }

    return bytes;
}

bool wallet_opening(
    const std::string& filepath,
    std::string* private_key_out,
    std::string* public_key_out,
    std::string* address_out) {
    std::ifstream infile(filepath);
    if (!infile.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(infile, line)) {
        if (line.rfind("Private key: ", 0) == 0) {
            if (private_key_out) {
                *private_key_out = line.substr(13);
            }
        } else if (line.rfind("Public key: ", 0) == 0) {
            if (public_key_out) {
                *public_key_out = line.substr(12);
            }
        } else if (line.rfind("Address: ", 0) == 0) {
            if (address_out) {
                *address_out = line.substr(9);
            }
        }
    }

    return true;
}