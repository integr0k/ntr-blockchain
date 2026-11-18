#include "node.hpp"
#include <algorithm>
#include <array>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr std::size_t max_message_size = 65536;

bool send_all(int socket_fd, const std::string& message) {
	std::size_t sent = 0;
	while (sent < message.size()) {
		const ssize_t result = send(socket_fd, message.data() + sent,
								   message.size() - sent, MSG_NOSIGNAL);
		if (result <= 0) {
			return false;
		}
		sent += static_cast<std::size_t>(result);
	}
	return true;
}

bool receive_line(int socket_fd, std::string& message) {
	std::array<char, 1024> buffer{};
	while (message.size() < max_message_size) {
		const ssize_t received = recv(socket_fd, buffer.data(), buffer.size(), 0);
		if (received <= 0) {
			return false;
		}
		for (ssize_t i = 0; i < received; ++i) {
			if (buffer[static_cast<std::size_t>(i)] == '\n') {
				return true;
			}
			message.push_back(buffer[static_cast<std::size_t>(i)]);
			if (message.size() >= max_message_size) {
				return false;
			}
		}
	}
	return false;
}
}

Node::~Node() {
	stop();
}

bool Node::start(uint16_t port) {
	if (port == 0 || running.load()) {
		return false;
	}
	const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		return false;
	}
	int reuse_address = 1;
	setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
		listen(socket_fd, 16) < 0) {
		close(socket_fd);
		return false;
	}

	server_socket.store(socket_fd);
	running.store(true);
	server_thread = std::thread(&Node::accept_connections, this);
	return true;
}

void Node::stop() {
	running.store(false);
	const int socket_fd = server_socket.exchange(-1);
	if (socket_fd >= 0) {
		shutdown(socket_fd, SHUT_RDWR);
		close(socket_fd);
	}
	if (server_thread.joinable()) {
		server_thread.join();
	}
}

bool Node::add_peer(const std::string& host, uint16_t port) {
	if (host.empty() || port == 0) {
		return false;
	}
	std::lock_guard<std::mutex> lock(peers_mutex);
	const auto duplicate = std::find_if(peers.begin(), peers.end(),
		[&host, port](const PeerEndpoint& peer) {
			return peer.host == host && peer.port == port;
		});
	if (duplicate != peers.end()) {
		return false;
	}
	peers.push_back({host, port});
	return true;
}

bool Node::remove_peer(const std::string& host, uint16_t port) {
	std::lock_guard<std::mutex> lock(peers_mutex);
	const auto previous_size = peers.size();
	peers.erase(std::remove_if(peers.begin(), peers.end(),
		[&host, port](const PeerEndpoint& peer) {
			return peer.host == host && peer.port == port;
		}), peers.end());
	return peers.size() != previous_size;
}

SubmissionResult Node::submit_transaction(const Transaction& transaction) {
	if (!transaction.verify_signature()) {
		return SubmissionResult::RejectedInvalid;
	}
	if (!mempool.add_transaction(transaction)) {
		return SubmissionResult::AlreadyKnown;
	}
	return broadcast_transaction(transaction)
		? SubmissionResult::AcceptedAndBroadcast
		: SubmissionResult::AcceptedLocally;
}

bool Node::receive_transaction(const std::string& serialized_transaction) {
	Transaction transaction;
	if (!Transaction::deserialize(serialized_transaction, transaction)) {
		return false;
	}
	const SubmissionResult result = submit_transaction(transaction);
	return result == SubmissionResult::AcceptedLocally ||
		   result == SubmissionResult::AcceptedAndBroadcast;
}

bool Node::broadcast_transaction(const Transaction& transaction) const {
	std::vector<PeerEndpoint> peer_snapshot;
	{
		std::lock_guard<std::mutex> lock(peers_mutex);
		peer_snapshot = peers;
	}
	const std::string message = transaction.serialize() + "\n";
	bool delivered = false;
	for (const PeerEndpoint& peer : peer_snapshot) {
		delivered = send_to_peer(peer, message) || delivered;
	}
	return delivered;
}

bool Node::send_to_peer(const PeerEndpoint& peer, const std::string& message) const {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* addresses = nullptr;
	const std::string service = std::to_string(peer.port);
	if (getaddrinfo(peer.host.c_str(), service.c_str(), &hints, &addresses) != 0) {
		return false;
	}

	bool delivered = false;
	for (addrinfo* address = addresses; address && !delivered; address = address->ai_next) {
		const int socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (socket_fd < 0) {
			continue;
		}
		if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
			delivered = send_all(socket_fd, message);
			shutdown(socket_fd, SHUT_WR);
		}
		close(socket_fd);
	}
	freeaddrinfo(addresses);
	return delivered;
}

void Node::accept_connections() {
	while (running.load()) {
		const int listening_socket = server_socket.load();
		if (listening_socket < 0) {
			break;
		}
		const int client_socket = accept(listening_socket, nullptr, nullptr);
		if (client_socket < 0) {
			if (!running.load()) {
				break;
			}
			continue;
		}
		std::string message;
		if (receive_line(client_socket, message)) {
			receive_transaction(message);
		}
		close(client_socket);
	}
}

Mempool& Node::get_mempool() { return mempool; }
const Mempool& Node::get_mempool() const { return mempool; }
