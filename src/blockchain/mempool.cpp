#include "mempool.hpp"
#include <algorithm>

bool Mempool::add_transaction(const Transaction& transaction) {
	if (!transaction.verify_signature()) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex);
	const auto duplicate = std::find_if(transactions.begin(), transactions.end(),
		[&transaction](const Transaction& existing) {
			return existing.getTxID() == transaction.getTxID();
		});
	if (duplicate != transactions.end()) {
		return false;
	}
	transactions.push_back(transaction);
	return true;
}

bool Mempool::contains(const std::string& txid) const {
	std::lock_guard<std::mutex> lock(mutex);
	return std::any_of(transactions.begin(), transactions.end(),
		[&txid](const Transaction& transaction) { return transaction.getTxID() == txid; });
}

std::vector<Transaction> Mempool::get_transactions() const {
	std::lock_guard<std::mutex> lock(mutex);
	return transactions;
}

bool Mempool::remove_transaction(const std::string& txid) {
	std::lock_guard<std::mutex> lock(mutex);
	const auto previous_size = transactions.size();
	transactions.erase(std::remove_if(transactions.begin(), transactions.end(),
		[&txid](const Transaction& transaction) { return transaction.getTxID() == txid; }),
		transactions.end());
	return transactions.size() != previous_size;
}

std::size_t Mempool::size() const {
	std::lock_guard<std::mutex> lock(mutex);
	return transactions.size();
}
