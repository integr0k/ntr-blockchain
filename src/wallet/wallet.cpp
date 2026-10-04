#include "wallet.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <vector>

void Wallet::b32_polymod_step(uint32_t& chk) {
    uint32_t b = chk >> 25;
    chk = (chk & 0x1ffffff) << 5;

    if ((b >> 0) & 1) chk ^= 0x3b6a57b2;
    if ((b >> 1) & 1) chk ^= 0x26508e6d;
    if ((b >> 2) & 1) chk ^= 0x1ea119fa;
    if ((b >> 3) & 1) chk ^= 0x3d4233dd;
    if ((b >> 4) & 1) chk ^= 0x2a1462b3;
}

bool Wallet::convert_bits(const std::vector<unsigned char>& in_data,
                         int from_bits,
                         int to_bits,
                         bool pad,
                         std::vector<unsigned char>& out_data) {
    uint32_t acc = 0;
    int bits = 0;
    uint32_t maxv = (1u << to_bits) - 1u;
    uint32_t max_acc = (1u << (from_bits + to_bits - 1)) - 1u;

    for (unsigned char value : in_data) {
        acc = ((acc << from_bits) | value) & max_acc;
        bits += from_bits;

        while (bits >= to_bits) {
            bits -= to_bits;
            out_data.push_back((acc >> bits) & maxv);
        }
    }

    if (pad) {
        if (bits) {
            out_data.push_back((acc << (to_bits - bits)) & maxv);
        }
        return true;
    }

    if (bits >= from_bits || ((acc << (to_bits - bits)) & maxv)) {
        return false;
    }

    return true;
}

std::string Wallet::bech32_encode(const std::string& htr,
                                 const std::vector<unsigned char>& data) {
    static const char* charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    uint32_t chk = 1;

    for (char c : htr) {
        chk = (chk << 5) ^ (static_cast<unsigned char>(c) >> 5);
        b32_polymod_step(chk);
    }

    chk ^= 1;
    for (char c : htr) {
        chk = (chk << 5) ^ (static_cast<unsigned char>(c) & 31u);
        b32_polymod_step(chk);
    }

    for (unsigned char v : data) {
        chk = (chk << 5) ^ v;
        b32_polymod_step(chk);
    }

    for (int p = 0; p < 6; ++p) {
        chk = (chk << 5);
        b32_polymod_step(chk);
    }

    chk ^= 1;

    std::string result = htr + "1";
    for (unsigned char v : data) {
        result += charset[v];
    }

    for (int i = 0; i < 6; ++i) {
        result += charset[(chk >> ((5 - i) * 5)) & 31];
    }

    return result;
}

bool Wallet::generate_private_key() {
    int result = RAND_bytes(private_key_bytes.data(), static_cast<int>(private_key_bytes.size()));
    if (result != 1) {
        std::cerr << "Error generate private key" << std::endl;
        return false;
    }

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char byte : private_key_bytes) {
        ss << std::setw(2) << static_cast<int>(byte);
    }

    private_key_hex = ss.str();
    return true;
}

bool Wallet::generate_public_key() {
    EC_KEY* ec_key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* bn_private_key = BN_new();

    if (!ec_key || !ctx || !bn_private_key) {
        std::cerr << "Error: OpenSSL allocation failed" << std::endl;
        BN_free(bn_private_key);
        BN_CTX_free(ctx);
        EC_KEY_free(ec_key);
        return false;
    }

    BN_bin2bn(private_key_bytes.data(), static_cast<int>(private_key_bytes.size()), bn_private_key);
    EC_KEY_set_private_key(ec_key, bn_private_key);

    const EC_GROUP* group = EC_KEY_get0_group(ec_key);
    EC_POINT* pub_point = EC_POINT_new(group);
    if (!pub_point) {
        std::cerr << "Error: EC_POINT allocation failed" << std::endl;
        BN_free(bn_private_key);
        BN_CTX_free(ctx);
        EC_KEY_free(ec_key);
        return false;
    }

    EC_POINT_mul(group, pub_point, bn_private_key, nullptr, nullptr, ctx);
    EC_KEY_set_public_key(ec_key, pub_point);

    if (EC_KEY_check_key(ec_key) != 1) {
        std::cerr << "Error: EC KEY check key" << std::endl;
        EC_POINT_free(pub_point);
        BN_free(bn_private_key);
        BN_CTX_free(ctx);
        EC_KEY_free(ec_key);
        return false;
    }

    unsigned char* raw_pub_bytes = nullptr;
    size_t bytes_len = EC_POINT_point2buf(group, pub_point, POINT_CONVERSION_UNCOMPRESSED, &raw_pub_bytes, ctx);
    if (bytes_len == 0 || raw_pub_bytes == nullptr) {
        std::cerr << "Error: failed to serialize public key" << std::endl;
        EC_POINT_free(pub_point);
        BN_free(bn_private_key);
        BN_CTX_free(ctx);
        EC_KEY_free(ec_key);
        return false;
    }

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes_len; ++i) {
        ss << std::setw(2) << static_cast<int>(raw_pub_bytes[i]);
    }
    public_key_hex = ss.str();
    public_key_bytes.assign(raw_pub_bytes, raw_pub_bytes + bytes_len);

    EC_POINT_free(pub_point);
    OPENSSL_free(raw_pub_bytes);
    BN_free(bn_private_key);
    BN_CTX_free(ctx);
    EC_KEY_free(ec_key);
    return true;
}

bool Wallet::generate_NTR_address() {
    std::vector<unsigned char> bits5_data;
    std::array<unsigned char, 32> sha256_result{};
    std::array<unsigned char, 20> ripemd160_result{};

    SHA256(public_key_bytes.data(), public_key_bytes.size(), sha256_result.data());
    RIPEMD160(sha256_result.data(), sha256_result.size(), ripemd160_result.data());

    std::vector<unsigned char> to_convert(ripemd160_result.begin(), ripemd160_result.end());
    convert_bits(to_convert, 8, 5, true, bits5_data);
    NTR_address_str = bech32_encode("ntr", bits5_data);
    return true;
}

bool Wallet::check_wallet() {
    return std::filesystem::exists("wallets");
}

bool Wallet::save_wallet() {
    std::filesystem::create_directory("wallets");

    std::filesystem::path full_path;
    int counter = 1;

    while (true) {
        std::string filename = "wallet" + std::to_string(counter) + ".txt";
        full_path = std::filesystem::path("wallets") / filename;
        if (!std::filesystem::exists(full_path)) {
            break;
        }
        ++counter;
    }

    std::ofstream outfile(full_path);
    if (!outfile.is_open()) {
        std::cerr << "Error: cannot create wallet file at: " << full_path << std::endl;
        return false;
    }

    outfile << "=== NTR WALLET #" << counter << " ===\n";
    outfile << "Private key: " << private_key_hex << "\n";
    outfile << "Public key: " << public_key_hex << "\n";
    outfile << "Address: " << NTR_address_str << "\n";
    outfile.close();

    std::cout << "[Success] Saved as: " << full_path.string() << std::endl;
    return true;
}

bool Wallet::wallet_opening_initialization(const std::string& filepath) {
    std::ifstream infile(filepath);
    if (!infile.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(infile, line)) {
        if (line.rfind("Private key: ", 0) == 0) {
            private_key_hex = line.substr(13);
        } else if (line.rfind("Public key: ", 0) == 0) {
            public_key_hex = line.substr(12);
        } else if (line.rfind("Address: ", 0) == 0) {
            NTR_address_str = line.substr(9);
        }
    }

    return true;
}

Wallet::Wallet() {
    private_key_hex.clear();
    public_key_hex.clear();
    NTR_address_str.clear();
    public_key_bytes.clear();
    private_key_bytes.fill(0);

    if (generate_private_key() && generate_public_key()) {
        generate_NTR_address();
        save_wallet();
    }
}

std::string Wallet::get_private_key_hex() const {
    return private_key_hex;
}

std::string Wallet::get_public_key_hex() const {
    return public_key_hex;
}

std::string Wallet::get_address() const {
    return NTR_address_str;
}

// int main()
// {
//     std::cout << "NTR_0.1.0 node skeleton\n"
//               << std::endl;

//     Wallet new_wallet;

//     std::cout << "wallet2.txt:" << std::endl << wallet_opening("wallets/wallet2.txt") << std::endl;

//     std::cout << "PRV_Key:     " << new_wallet.get_private_key_hex() << std::endl;
//     std::cout << "PUB_Key:     " << new_wallet.get_public_key_hex() << std::endl;
//     std::cout << "NTR_Address: " << new_wallet.get_address() << std::endl;

//     return 0;
// }