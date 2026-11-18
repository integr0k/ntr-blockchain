#ifndef NTR_NODE_HPP
#define NTR_NODE_HPP
#include "../blockchain/mempool.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class SubmissionResult {
	RejectedInvalid,
	AlreadyKnown,
	AcceptedLocally,
	AcceptedAndBroadcast
};

struct PeerEndpoint {
	std::string host;
	uint16_t port;
};

class Node {
private:
	Mempool mempool;
	std::vector<PeerEndpoint> peers;
	mutable std::mutex peers_mutex;
	std::atomic<int> server_socket{-1};
	std::atomic<bool> running{false};
	std::thread server_thread;

	void accept_connections();
	bool send_to_peer(const PeerEndpoint& peer, const std::string& message) const;
	bool broadcast_transaction(const Transaction& transaction) const;

public:
	Node() = default;
	~Node();
	Node(const Node&) = delete;
	Node& operator=(const Node&) = delete;

	bool start(uint16_t port);
	void stop();
	bool add_peer(const std::string& host, uint16_t port);
	bool remove_peer(const std::string& host, uint16_t port);
	SubmissionResult submit_transaction(const Transaction& transaction);
	bool receive_transaction(const std::string& serialized_transaction);
	Mempool& get_mempool();
	const Mempool& get_mempool() const;
};

#endif
