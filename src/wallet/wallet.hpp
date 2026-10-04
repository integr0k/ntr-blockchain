#ifndef WALLET_HPP
#define WALLET_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct bignum_st;
using BIGNUM = struct bignum_st;

struct ec_key_st;
using EC_KEY = struct ec_key_st;

class Wallet {
private:
    std::array<unsigned char, 32> private_key_bytes{};
    std::string private_key_hex;
    std::vector<unsigned char> public_key_bytes;
    std::string public_key_hex;
    std::string NTR_address_str;

    void b32_polymod_step(uint32_t& chk);

    bool convert_bits(const std::vector<unsigned char>& in_data,
                      int from_bits,
                      int to_bits,
                      bool pad,
                      std::vector<unsigned char>& out_data);

    std::string bech32_encode(const std::string& htr,
                              const std::vector<unsigned char>& data);

    bool generate_private_key();
    bool generate_public_key();
    bool generate_NTR_address();

public:
    bool check_wallet();
    bool save_wallet();
    bool wallet_opening_initialization(const std::string& filepath);

    Wallet();

    std::string get_private_key_hex() const;
    std::string get_public_key_hex() const;
    std::string get_address() const;
};

#endif
