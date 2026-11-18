#include "transaction.hpp"
#include "../../wallet/wallet.hpp"
#include <algorithm>
#include <iomanip>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/ecdsa.h>
#include <openssl/sha.h>
#include <sstream>
#include <type_traits>

namespace {
template <typename Integer>
void append_little_endian(std::vector<uint8_t>& bytes, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned encoded = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(Integer); ++i) {
        bytes.push_back(static_cast<uint8_t>(encoded & 0xff));
        encoded >>= 8;
    }
}

void append_string(std::vector<uint8_t>& bytes, const std::string& value) {
    append_little_endian(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

bool is_hex(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    });
}

std::string bn_to_fixed_hex(const BIGNUM* value) {
    char* encoded = BN_bn2hex(value);
    if (!encoded) {
        return {};
    }
    std::string result(encoded);
    OPENSSL_free(encoded);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (result.size() > 64) {
        return {};
    }
    return std::string(64 - result.size(), '0') + result;
}
}

Transaction::Transaction(const std::string& from, const std::string& to,
                         uint64_t value, uint64_t transaction_nonce, int64_t time)
    : timestamp(time), amount(value), nonce(transaction_nonce), sender(from), recipient(to) {
    calculate_txid();
}

std::string Transaction::sha256(const std::vector<uint8_t>& bytes) const {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(bytes.data(), bytes.size(), hash);
    std::stringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char byte : hash) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

std::vector<uint8_t> Transaction::get_signing_bytes() const {
    std::vector<uint8_t> bytes;
    append_little_endian(bytes, version);
    append_little_endian(bytes, amount);
    append_little_endian(bytes, timestamp);
    append_little_endian(bytes, nonce);
    append_little_endian(bytes, fee);
    append_little_endian(bytes, fee_limit);
    append_string(bytes, sender);
    append_string(bytes, recipient);
    append_string(bytes, public_key);
    return bytes;
}

bool Transaction::generate_signature(const std::string& private_key_hex) {
    if (private_key_hex.empty() || !is_hex(private_key_hex)) {
        return false;
    }

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    BIGNUM* private_key = nullptr;
    BN_CTX* context = BN_CTX_new();
    if (!key || !context || BN_hex2bn(&private_key, private_key_hex.c_str()) == 0 || !private_key) {
        EC_KEY_free(key);
        BN_CTX_free(context);
        BN_clear_free(private_key);
        return false;
    }

    bool success = false;
    const EC_GROUP* group = EC_KEY_get0_group(key);
    EC_POINT* public_point = EC_POINT_new(group);
    if (!public_point || EC_KEY_set_private_key(key, private_key) != 1 ||
        EC_POINT_mul(group, public_point, private_key, nullptr, nullptr, context) != 1 ||
        EC_KEY_set_public_key(key, public_point) != 1 || EC_KEY_check_key(key) != 1) {
        EC_POINT_free(public_point);
        EC_KEY_free(key);
        BN_CTX_free(context);
        BN_clear_free(private_key);
        return false;
    }

    unsigned char* public_bytes = nullptr;
    const size_t public_size = EC_POINT_point2buf(
        group, public_point, POINT_CONVERSION_UNCOMPRESSED, &public_bytes, context);
    if (public_size == 0 || !public_bytes) {
        EC_POINT_free(public_point);
        EC_KEY_free(key);
        BN_CTX_free(context);
        BN_clear_free(private_key);
        return false;
    }

    std::ostringstream public_hex;
    public_hex << std::hex << std::setfill('0');
    for (size_t i = 0; i < public_size; ++i) {
        public_hex << std::setw(2) << static_cast<unsigned int>(public_bytes[i]);
    }
    public_key = public_hex.str();
    OPENSSL_free(public_bytes);
    EC_POINT_free(public_point);

    if (Wallet::address_from_public_key_hex(public_key) != sender) {
        public_key.clear();
        EC_KEY_free(key);
        BN_CTX_free(context);
        BN_clear_free(private_key);
        return false;
    }

    const std::vector<uint8_t> message = get_signing_bytes();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(message.data(), message.size(), digest);
    ECDSA_SIG* raw_signature = ECDSA_do_sign(digest, sizeof(digest), key);
    if (raw_signature) {
        const BIGNUM* r = nullptr;
        const BIGNUM* s = nullptr;
        ECDSA_SIG_get0(raw_signature, &r, &s);
        const std::string r_hex = bn_to_fixed_hex(r);
        const std::string s_hex = bn_to_fixed_hex(s);
        if (!r_hex.empty() && !s_hex.empty()) {
            signature = r_hex + s_hex;
            calculate_txid();
            success = true;
        }
        ECDSA_SIG_free(raw_signature);
    }

    EC_KEY_free(key);
    BN_CTX_free(context);
    BN_clear_free(private_key);
    return success;
}

bool Transaction::verify_signature() const {
    if (amount == 0 || signature.size() != 128 || !is_hex(signature) ||
        public_key.size() != 130 || !is_hex(public_key) || sender.empty() ||
        recipient.empty() || Wallet::address_from_public_key_hex(public_key) != sender) {
        return false;
    }

    EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
    const std::vector<uint8_t> public_bytes = [&]() {
        std::vector<uint8_t> result;
        for (std::size_t i = 0; i < public_key.size(); i += 2) {
            result.push_back(static_cast<uint8_t>(std::stoul(public_key.substr(i, 2), nullptr, 16)));
        }
        return result;
    }();
    const EC_GROUP* group = key ? EC_KEY_get0_group(key) : nullptr;
    EC_POINT* point = group ? EC_POINT_new(group) : nullptr;
    BN_CTX* context = BN_CTX_new();
    BIGNUM* r = nullptr;
    BIGNUM* s = nullptr;
    ECDSA_SIG* parsed_signature = ECDSA_SIG_new();
    bool valid = false;

    if (key && point && context && parsed_signature &&
        EC_POINT_oct2point(group, point, public_bytes.data(), public_bytes.size(), context) == 1 &&
        EC_KEY_set_public_key(key, point) == 1 &&
        BN_hex2bn(&r, signature.substr(0, 64).c_str()) > 0 &&
        BN_hex2bn(&s, signature.substr(64, 64).c_str()) > 0 &&
        ECDSA_SIG_set0(parsed_signature, r, s) == 1) {
        r = nullptr;
        s = nullptr;
        const std::vector<uint8_t> message = get_signing_bytes();
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(message.data(), message.size(), digest);
        valid = ECDSA_do_verify(digest, sizeof(digest), parsed_signature, key) == 1;
    }

    BN_free(r);
    BN_free(s);
    ECDSA_SIG_free(parsed_signature);
    EC_POINT_free(point);
    BN_CTX_free(context);
    EC_KEY_free(key);
    return valid;
}

void Transaction::calculate_txid() {
    std::vector<uint8_t> bytes = get_signing_bytes();
    if (signature.size() % 2 == 0 && is_hex(signature)) {
        for (std::size_t i = 0; i < signature.size(); i += 2) {
            bytes.push_back(static_cast<uint8_t>(std::stoul(signature.substr(i, 2), nullptr, 16)));
        }
    }
    txid = sha256(bytes);
}

std::string Transaction::serialize() const {
    std::ostringstream stream;
    stream << version << ' ' << amount << ' ' << timestamp << ' ' << nonce << ' '
           << fee << ' ' << fee_limit << ' ' << std::quoted(sender) << ' '
           << std::quoted(recipient) << ' ' << std::quoted(public_key) << ' '
           << std::quoted(signature) << ' ' << std::quoted(txid);
    return stream.str();
}

bool Transaction::deserialize(const std::string& data, Transaction& transaction) {
    std::istringstream stream(data);
    Transaction parsed;
    if (!(stream >> parsed.version >> parsed.amount >> parsed.timestamp >> parsed.nonce
          >> parsed.fee >> parsed.fee_limit >> std::quoted(parsed.sender)
          >> std::quoted(parsed.recipient) >> std::quoted(parsed.public_key)
          >> std::quoted(parsed.signature) >> std::quoted(parsed.txid))) {
        return false;
    }
    stream >> std::ws;
    if (!stream.eof() || parsed.version != 1 || !parsed.verify_signature()) {
        return false;
    }
    const std::string transmitted_txid = parsed.txid;
    parsed.calculate_txid();
    if (parsed.txid != transmitted_txid) {
        return false;
    }
    transaction = std::move(parsed);
    return true;
}

const std::string& Transaction::getTxID() const { return txid; }
const std::string& Transaction::getSignature() const { return signature; }
const std::string& Transaction::get_sender() const { return sender; }
const std::string& Transaction::get_recipient() const { return recipient; }
uint64_t Transaction::get_amount() const { return amount; }
uint64_t Transaction::get_fee() const { return fee; }
uint64_t Transaction::get_nonce() const { return nonce; }