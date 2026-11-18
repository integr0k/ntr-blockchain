#ifndef NTR_MEMPOOL_HPP
#define NTR_MEMPOOL_HPP
#include "transactions/transaction.hpp"
#include <mutex>
#include <string>
#include <vector>

class Mempool {
private:
	std::vector<Transaction> transactions;
	mutable std::mutex mutex;

public:
	bool add_transaction(const Transaction& transaction);
	bool contains(const std::string& txid) const;
	std::vector<Transaction> get_transactions() const;
	bool remove_transaction(const std::string& txid);
	std::size_t size() const;
};

#endif
