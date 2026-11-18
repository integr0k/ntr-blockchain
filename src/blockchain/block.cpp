#include "block.hpp"
#include <chrono>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>

namespace {
const std::string zero_hash(64, '0');

int64_t current_timestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

BlockOfBlockChain::BlockOfBlockChain()
    : BlockOfBlockChain(0, zero_hash, current_timestamp()) {}

BlockOfBlockChain::BlockOfBlockChain(uint64_t block_index,
                                     const std::string& previous_hash,
                                                                         int64_t block_timestamp, uint64_t block_nonce,
                                                                         const std::string& block_miner, uint64_t block_reward,
                                                                         uint32_t block_difficulty, uint32_t block_header_version,
                                                                         uint64_t block_fees)
        : previous_block_hash(previous_hash), miner_address(block_miner),
            timestamp(block_timestamp), index(block_index), nonce(block_nonce),
            reward(block_reward), fees(block_fees), difficulty(block_difficulty),
            header_version(block_header_version) {
    calculate_merkle_root();
    calculate_hash();
}

std::string BlockOfBlockChain::sha256(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char byte : digest) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

BlockOfBlockChain BlockOfBlockChain::create_genesis_block() {
    return BlockOfBlockChain(0, zero_hash, 0, 0, "", 0, 0, 0);
}

void BlockOfBlockChain::calculate_merkle_root() {
    std::vector<std::string> level;
    level.reserve(transactions.size());
    for (const Transaction& transaction : transactions) {
        level.push_back(transaction.getTxID());
    }
    if (level.empty()) {
        merkle_root = sha256("");
        return;
    }
    while (level.size() > 1) {
        std::vector<std::string> next_level;
        for (std::size_t i = 0; i < level.size(); i += 2) {
            const std::string& right = i + 1 < level.size() ? level[i + 1] : level[i];
            next_level.push_back(sha256(level[i] + right));
        }
        level = std::move(next_level);
    }
    merkle_root = level.front();
}

void BlockOfBlockChain::calculate_hash() {
    if (index == 0) {
        block_hash = sha256("NTR local test network genesis v1");
        return;
    }
    std::ostringstream header;
    if (header_version == 1) {
        header << index << ':' << previous_block_hash << ':' << timestamp << ':'
               << nonce << ':' << miner_address << ':' << reward;
    } else if (header_version == 2) {
        header << header_version << ':' << index << ':' << previous_block_hash << ':'
               << timestamp << ':' << nonce << ':' << miner_address << ':' << reward
               << ':' << difficulty << ':' << merkle_root;
    } else {
        header << header_version << ':' << index << ':' << previous_block_hash << ':'
               << timestamp << ':' << nonce << ':' << miner_address << ':' << reward
             << ':' << fees << ':' << difficulty << ':' << merkle_root;
    }
    block_hash = sha256(header.str());
}

void BlockOfBlockChain::set_mining_data(const std::string& address,
                                        uint64_t block_reward,
                                        uint32_t block_difficulty,
                                        uint64_t block_fees) {
    miner_address = address;
    reward = block_reward;
    fees = block_fees;
    difficulty = block_difficulty;
    header_version = 4;
    nonce = 0;
    calculate_hash();
}

void BlockOfBlockChain::set_nonce(uint64_t block_nonce) {
    nonce = block_nonce;
    calculate_hash();
}

bool BlockOfBlockChain::has_valid_proof() const {
    if (index == 0) {
        return true;
    }
    if (difficulty == 0 || difficulty > block_hash.size()) {
        return false;
    }
    return block_hash.compare(0, difficulty, std::string(difficulty, '0')) == 0;
}

bool BlockOfBlockChain::add_transaction(const Transaction& transaction) {
    if (!transaction.verify_signature()) {
        return false;
    }
    for (const Transaction& existing : transactions) {
        if (existing.getTxID() == transaction.getTxID()) {
            return false;
        }
    }
    transactions.push_back(transaction);
    calculate_merkle_root();
    calculate_hash();
    return true;
}

uint64_t BlockOfBlockChain::get_index() const { return index; }
uint64_t BlockOfBlockChain::get_nonce() const { return nonce; }
uint64_t BlockOfBlockChain::get_reward() const { return reward; }
uint64_t BlockOfBlockChain::get_fees() const { return fees; }
uint32_t BlockOfBlockChain::get_difficulty() const { return difficulty; }
uint32_t BlockOfBlockChain::get_header_version() const { return header_version; }
int64_t BlockOfBlockChain::get_timestamp() const { return timestamp; }
const std::string& BlockOfBlockChain::get_miner_address() const { return miner_address; }
const std::string& BlockOfBlockChain::get_previous_hash() const { return previous_block_hash; }
const std::string& BlockOfBlockChain::get_merkle_root() const { return merkle_root; }
const std::string& BlockOfBlockChain::get_hash() const { return block_hash; }
const std::vector<Transaction>& BlockOfBlockChain::get_transactions() const { return transactions; }