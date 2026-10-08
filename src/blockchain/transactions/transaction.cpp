#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/sha.h>

class Transaction
{
private:
    uint16_t version;       // Версия TRX для будущих обновлений сети
    uint64_t fee;
    uint64_t fee_limit;
    int64_t timestamp;
    uint64_t amount;
    uint64_t nonce;
    std::string sender;
    std::string recipient;
    std::string signature;  // Подпись (r + s в hex, строго 128 символов)
    std::string txid;       // Итоговый TXID

    // Чистая функция SHA-256 для байтовых массивов
    std::string sha256(const std::vector<uint8_t>& bytes) const {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(bytes.data(), bytes.size(), hash);
        
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

public:
    // Конструктор: просто принимает и записывает всё, что передали (без проверок)
    Transaction(const std::string &from, const std::string &to, uint64_t val, uint64_t n, int64_t time)
        : version(1), fee(10), fee_limit(1000), timestamp(time), amount(val), nonce(n), sender(from), recipient(to), signature(""), txid("")
    {
        calculate_txid(); 
    }

    // Побайтовая сборка полей для подписи (Little-Endian)
    std::vector<uint8_t> get_signing_bytes() const {
        std::vector<uint8_t> bytes;

        auto append_bytes = [&](const auto& value) {
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
            bytes.insert(bytes.end(), ptr, ptr + sizeof(value));
        };

        // Сериализуем фиксированные числа
        append_bytes(version);
        append_bytes(amount);
        append_bytes(timestamp);
        append_bytes(nonce);
        append_bytes(fee);
        append_bytes(fee_limit);

        // Сериализуем строки (длина + контент)
        uint32_t sender_len = static_cast<uint32_t>(sender.length());
        append_bytes(sender_len);
        bytes.insert(bytes.end(), sender.begin(), sender.end());

        uint32_t recipient_len = static_cast<uint32_t>(recipient.length());
        append_bytes(recipient_len);
        bytes.insert(bytes.end(), recipient.begin(), recipient.end());

        return bytes;
    }

    // Вычисление подписи OpenSSL
    bool generate_signature(const std::string& private_key_hex) {
        EC_KEY *ec_key = EC_KEY_new_by_curve_name(NID_secp256k1);
        BIGNUM *bn_private_key = nullptr;

        if (BN_hex2bn(&bn_private_key, private_key_hex.c_str()) == 0 || bn_private_key == nullptr) {
            EC_KEY_free(ec_key);
            return false;
        }
        EC_KEY_set_private_key(ec_key, bn_private_key);

        // Хэшируем бинарный пакет данных
        std::string msg_hash_hex = sha256(get_signing_bytes());
        
        unsigned char raw_hash[32];
        for (size_t i = 0; i < 32; ++i) {
            std::string byteString = msg_hash_hex.substr(i * 2, 2);
            raw_hash[i] = static_cast<unsigned char>(std::stoul(byteString, nullptr, 16));
        }

        ECDSA_SIG *sig = ECDSA_do_sign(raw_hash, 32, ec_key);
        if (sig == nullptr) {
            BN_clear_free(bn_private_key);
            EC_KEY_free(ec_key);
            return false;
        }

        const BIGNUM *r = nullptr;
        const BIGNUM *s = nullptr;
        ECDSA_SIG_get0(sig, &r, &s);

        char *r_hex = BN_bn2hex(r);
        char *s_hex = BN_bn2hex(s);

        std::string r_str(r_hex);
        std::string s_str(s_hex);

        // Математический padding: выравниваем R и S строго до 64 символов (32 байт)
        if (r_str.length() < 64) r_str = std::string(64 - r_str.length(), '0') + r_str;
        if (s_str.length() < 64) s_str = std::string(64 - s_str.length(), '0') + s_str;

        signature = r_str + s_str;
        std::transform(signature.begin(), signature.end(), signature.begin(), ::tolower);

        OPENSSL_free(r_hex);
        OPENSSL_free(s_hex);
        ECDSA_SIG_free(sig);
        BN_clear_free(bn_private_key);
        EC_KEY_free(ec_key);

        calculate_txid();
        return true;
    }

    // Калькуляция TXID на основе бинарных данных и бинарной подписи
    void calculate_txid() {
        std::vector<uint8_t> full_tx_bytes = get_signing_bytes();
        
        // Добавляем байты подписи в конец, если она уже сгенерирована
        if (!signature.empty()) {
            for (size_t i = 0; i < signature.length(); i += 2) {
                std::string byteString = signature.substr(i, 2);
                uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
                full_tx_bytes.push_back(byte);
            }
        }

        txid = sha256(full_tx_bytes);
    }

    // Геттеры для экспорта во внешние модули (валидаторы, кэш, ноды)
    std::string getTxID() const { return txid; };
    std::string getSignature() const { return signature; };
};