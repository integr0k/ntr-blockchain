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
    std::string sha256(const std::vector<uint8_t>& bytes) const;

    // Калькуляция TXID на основе бинарных данных и бинарной подписи
    void calculate_txid();

public:
    // Конструктор: просто принимает и записывает всё, что передали (без проверок)
    Transaction(const std::string &from, const std::string &to, uint64_t val, uint64_t n, int64_t time);

    // Дефолтный конструктор (нужен, если будете сохранять объекты в массивы/контейнеры)
    Transaction() = default;

    // Побайтовая сборка полей для подписи (Little-Endian)
    std::vector<uint8_t> get_signing_bytes() const;

    // Вычисление подписи OpenSSL
    bool generate_signature(const std::string& private_key_hex);

    // Геттеры для экспорта во внешние модули (валидаторы, кэш, ноды)
    std::string getTxID() const;
    std::string getSignature() const;
};

