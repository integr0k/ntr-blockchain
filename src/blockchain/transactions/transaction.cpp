#include <wallet.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <array>
#include <sstream>
#include <chrono>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/ripemd.h>

class Transaction
{
private:
    int64_t timestamp;
    uint64_t amount;
    uint64_t nonce;
    std::string sender;
    std::string recipient;
    std::string signature;
    std::string txid;

public:
    std::string to_string() const
    {
        std::stringstream ss;
        ss << sender << ":" << amount << ":" << recipient << ":" << timestamp << ":" << nonce;
        return ss.str();
    }

    void calculate_hash()
    {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        std::string dataData = to_string();
        std::vector<unsigned char> bytes_vector_data(dataData.begin(), dataData.end());

        SHA256(reinterpret_cast<const unsigned char *>(dataData.c_str()), dataData.length(), hash);

        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++)
        {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }

        txid = ss.str();
    }

    Transaction(const std::string &from, const std::string &to, uint64_t val, uint64_t n, int64_t time)
        : sender(from), recipient(to), amount(val), nonce(n), timestamp(time)
    {

        calculate_hash();
    }

    std::string getTxID() const { return txid; };
    std::string getSender() const { return sender; };
};



void process_transaction() {
    Wallet sender;
    // Просто передаем путь к нужному файлу кошелька
    if (sender.wallet_opening_initialization("src/crypto/wallets/wallet1.txt")) {
        std::cout << "Отправитель: " << sender.get_address() << " готов к отправке!\n";
    }
}

int main()
{
    std::cout << "NTR_0.1.0 node skeleton\n" << std::endl;
    Transaction new_transaction("malah", "mne", 19, 1123, 123);
    std::cout << "Transaction: " << new_transaction.to_string() << std::endl;
    std::cout << "Hash: " << new_transaction.getTxID() << std::endl;
}