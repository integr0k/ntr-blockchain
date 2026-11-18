#ifndef NTR_BLOCK_HPP
#define NTR_BLOCK_HPP
#include "transactions/transaction.hpp"
#include <cstdint>
#include <string>
#include <vector>

class BlockOfBlockChain {
private:
	std::vector<Transaction> transactions;
	std::string merkle_root;
	std::string block_hash;
	std::string previous_block_hash;
	std::string miner_address;
	int64_t timestamp{0};
	uint64_t index{0};
	uint64_t nonce{0};
	uint64_t reward{0};
	uint64_t fees{0};
	uint32_t difficulty{0};
	uint32_t header_version{2};

	static std::string sha256(const std::string& value);
	void calculate_merkle_root();

public:
	BlockOfBlockChain();
	BlockOfBlockChain(uint64_t block_index, const std::string& previous_hash,
					  int64_t block_timestamp, uint64_t block_nonce = 0,
					  const std::string& block_miner = "", uint64_t block_reward = 0,
					  uint32_t block_difficulty = 0, uint32_t block_header_version = 2,
					  uint64_t block_fees = 0);

	static BlockOfBlockChain create_genesis_block();
	bool add_transaction(const Transaction& transaction);
	void set_mining_data(const std::string& address, uint64_t block_reward,
						 uint32_t block_difficulty, uint64_t block_fees = 0);
	void set_nonce(uint64_t block_nonce);
	void calculate_hash();
	bool has_valid_proof() const;

	uint64_t get_index() const;
	uint64_t get_nonce() const;
	uint64_t get_reward() const;
	uint64_t get_fees() const;
	uint32_t get_difficulty() const;
	uint32_t get_header_version() const;
	int64_t get_timestamp() const;
	const std::string& get_miner_address() const;
	const std::string& get_previous_hash() const;
	const std::string& get_merkle_root() const;
	const std::string& get_hash() const;
	const std::vector<Transaction>& get_transactions() const;
};

#endif
