#ifndef NTR_BLOCKCHAIN_HPP
#define NTR_BLOCKCHAIN_HPP
#include "block.hpp"
#include <vector>

class Blockchain {
private:
	std::vector<BlockOfBlockChain> blocks;
	static constexpr uint32_t initial_difficulty = 5;
	static constexpr uint32_t minimum_difficulty = 3;
	static constexpr uint32_t maximum_difficulty = 7;
	static constexpr uint32_t difficulty_window = 6;
	static constexpr int64_t target_block_seconds = 12;

public:
	Blockchain();

	BlockOfBlockChain create_genesis_block();
	uint32_t get_next_difficulty() const;
	bool add_block(const BlockOfBlockChain& block);
	const BlockOfBlockChain& get_latest_block() const;
	const std::vector<BlockOfBlockChain>& get_blocks() const;
};

#endif
