#ifndef NTR_DASHBOARD_SERVER_HPP
#define NTR_DASHBOARD_SERVER_HPP
#include "../blockchain/blockchain.hpp"
#include "../network/node.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <vector>

class DashboardServer {
private:
    struct HttpRequest {
        std::string method;
        std::string path;
        std::string query;
        std::string body;
    };

    sqlite3* database{nullptr};
    int server_socket{-1};
    uint16_t port{4173};
    uint64_t block_reward{50000000};
    std::atomic<bool> server_running{false};
    std::atomic<bool> mining_enabled{false};
    std::thread mining_thread;
    Blockchain blockchain;
    Node network_node;
    std::string miner_address;
    std::string candidate_hash{"waiting for miner"};
    uint64_t nonce{0};
    uint64_t hashes{0};
    uint64_t hash_rate{0};
    uint64_t last_reward_height{0};
    int64_t last_reward_timestamp{0};
    bool has_last_reward{false};
    std::vector<uint64_t> hash_rate_samples;
    std::mutex database_mutex;
    std::mutex blockchain_mutex;
    std::mutex mining_mutex;

    bool initialize_database();
    bool load_blockchain();
    bool load_saved_peers();
    bool load_pending_transactions();
    void server_loop();
    void mining_loop();
    void handle_client(int client_socket);
    bool read_request(int client_socket, HttpRequest& request);
    void handle_request(int client_socket, const HttpRequest& request);
    void send_response(int client_socket, int status, const std::string& content_type,
                       const std::string& body);
    std::string build_state_json();
    std::string build_transactions_json(unsigned int limit);
    std::string build_wallets_json();
    std::string build_mempool_json();
    std::string get_json_string(const std::string& body, const std::string& key);
    int64_t get_json_integer(const std::string& body, const std::string& key);
    bool get_json_boolean(const std::string& body, const std::string& key);
    bool valid_address(const std::string& address) const;
    bool valid_wallet_file(const std::string& wallet_file) const;
    bool create_wallet();
    void submit_wallet_transaction(int client_socket, const HttpRequest& request);
    bool start_mining(const std::string& address);
    void stop_mining();
    bool add_peer(const std::string& host, uint16_t peer_port);
    bool probe_peer(int peer_id, const std::string& host, uint16_t peer_port);
    bool delete_peer(int peer_id);
    static std::string json_escape(const std::string& value);
    static std::string status_text(int status);
    static std::string current_directory();

public:
    explicit DashboardServer(uint16_t listen_port = 4173);
    ~DashboardServer();
    DashboardServer(const DashboardServer&) = delete;
    DashboardServer& operator=(const DashboardServer&) = delete;

    bool initialize();
    bool run();
    void stop();
};

#endif
