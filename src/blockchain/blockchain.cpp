#include "blockchain.hpp"
#include <stdexcept>

Blockchain::Blockchain() {
	create_genesis_block();
}

BlockOfBlockChain Blockchain::create_genesis_block() {
	if (blocks.empty()) {
		blocks.push_back(BlockOfBlockChain::create_genesis_block());
	}
	return blocks.front();
}

uint32_t Blockchain::get_next_difficulty() const {
	if (blocks.empty() || blocks.back().get_header_version() < 4) {
		return initial_difficulty;
	}

	uint32_t recent_v4_blocks = 0;
	for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
		if (block->get_header_version() < 4) break;
		++recent_v4_blocks;
	}

	const uint32_t current_difficulty = blocks.back().get_difficulty();
	if (recent_v4_blocks < difficulty_window || recent_v4_blocks % difficulty_window != 0) {
		return current_difficulty;
	}

	const BlockOfBlockChain& first_block = blocks[blocks.size() - difficulty_window];
	const BlockOfBlockChain& latest_block = blocks.back();
	const int64_t actual_seconds = latest_block.get_timestamp() - first_block.get_timestamp();
	const int64_t target_seconds = target_block_seconds * (difficulty_window - 1);

	if (actual_seconds < target_seconds / 2 && current_difficulty < maximum_difficulty) {
		return current_difficulty + 1;
	}
	if (actual_seconds > target_seconds * 2 && current_difficulty > minimum_difficulty) {
		return current_difficulty - 1;
	}
	return current_difficulty;
}

bool Blockchain::add_block(const BlockOfBlockChain& block) {
	if (blocks.empty()) {
		return false;
	}
	const BlockOfBlockChain& previous = blocks.back();
	if (block.get_index() != previous.get_index() + 1 ||
		block.get_previous_hash() != previous.get_hash() ||
		block.get_header_version() > 4 ||
		(previous.get_header_version() >= 4 && block.get_header_version() < 4) ||
		(block.get_header_version() == 4 && block.get_difficulty() != get_next_difficulty()) ||
		!block.has_valid_proof()) {
		return false;
	}
	for (const Transaction& transaction : block.get_transactions()) {
		if (!transaction.verify_signature()) {
			return false;
		}
	}
	blocks.push_back(block);
	return true;
}

const BlockOfBlockChain& Blockchain::get_latest_block() const {
	if (blocks.empty()) {
		throw std::logic_error("Blockchain has no genesis block");
	}
	return blocks.back();
}

const std::vector<BlockOfBlockChain>& Blockchain::get_blocks() const {
	return blocks;
}
