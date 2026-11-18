#ifndef NTR_TRANSACTION_HPP
#define NTR_TRANSACTION_HPP
#include <cstdint>
#include <string>
#include <vector>

class Transaction
{
private:
    uint16_t version{1};
    uint64_t fee{10};
    uint64_t fee_limit{1000};
    int64_t timestamp{0};
    uint64_t amount{0};
    uint64_t nonce{0};
    std::string sender;
    std::string recipient;
    std::string public_key;
    std::string signature;
    std::string txid;

    std::string sha256(const std::vector<uint8_t>& bytes) const;
    void calculate_txid();

public:
    Transaction(const std::string& from, const std::string& to,
                uint64_t value, uint64_t transaction_nonce, int64_t time);
    Transaction() = default;

    std::vector<uint8_t> get_signing_bytes() const;
    bool generate_signature(const std::string& private_key_hex);
    bool verify_signature() const;
    std::string serialize() const;
    static bool deserialize(const std::string& data, Transaction& transaction);

    const std::string& getTxID() const;
    const std::string& getSignature() const;
    const std::string& get_sender() const;
    const std::string& get_recipient() const;
    uint64_t get_amount() const;
    uint64_t get_fee() const;
    uint64_t get_nonce() const;
};

#endif

