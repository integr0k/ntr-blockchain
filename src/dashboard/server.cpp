#include "server.hpp"
#include "../crypto/helper.hpp"
#include "../wallet/wallet.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>

#include <openssl/sha.h>

namespace {
const std::size_t max_request_size = 65536;
const std::string zero_hash(64, '0');

int64_t current_timestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string sha256_text(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char byte : digest) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

bool execute_sql(sqlite3* database, const char* query) {
    char* error_message = nullptr;
    const int result = sqlite3_exec(database, query, nullptr, nullptr, &error_message);
    if (result != SQLITE_OK) {
        std::cerr << "SQLite error: " << (error_message ? error_message : sqlite3_errmsg(database)) << std::endl;
        sqlite3_free(error_message);
        return false;
    }
    return true;
}

bool table_has_column(sqlite3* database, const char* table_name, const char* column_name) {
    const std::string query = std::string("PRAGMA table_info(") + table_name + ')';
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, query.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        if (name && std::string(name) == column_name) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    return found;
}

bool parse_ntr_amount(const std::string& value, uint64_t& amount) {
    if (value.empty()) return false;
    const std::size_t decimal_point = value.find('.');
    if (decimal_point != std::string::npos && value.find('.', decimal_point + 1) != std::string::npos) {
        return false;
    }
    const std::string whole_text = value.substr(0, decimal_point);
    const std::string fraction_text = decimal_point == std::string::npos
        ? std::string() : value.substr(decimal_point + 1);
    if (whole_text.empty() || fraction_text.size() > 6) return false;
    if (!std::all_of(whole_text.begin(), whole_text.end(), [](unsigned char ch) { return std::isdigit(ch); }) ||
        !std::all_of(fraction_text.begin(), fraction_text.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        return false;
    }
    try {
        const uint64_t whole = std::stoull(whole_text);
        std::string padded_fraction = fraction_text;
        padded_fraction.append(6 - padded_fraction.size(), '0');
        const uint64_t fraction = padded_fraction.empty() ? 0 : std::stoull(padded_fraction);
        if (whole > (std::numeric_limits<uint64_t>::max() - fraction) / 1000000) return false;
        amount = whole * 1000000 + fraction;
    } catch (const std::exception&) {
        return false;
    }
    return amount > 0;
}

bool send_all(int socket_fd, const std::string& value) {
    std::size_t sent = 0;
    while (sent < value.size()) {
        const ssize_t result = send(socket_fd, value.data() + sent,
                                    value.size() - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::string read_wallet_address() {
    std::ifstream wallet_file("wallets/wallet1.txt");
    std::string line;
    while (std::getline(wallet_file, line)) {
        if (line.rfind("Address: ", 0) == 0) {
            return line.substr(9);
        }
    }
    return "ntr1localtestminer000000000000000000";
}
}

DashboardServer::DashboardServer(uint16_t listen_port) : port(listen_port) {
    hash_rate_samples.assign(48, 0);
}

DashboardServer::~DashboardServer() {
    stop();
    if (database) {
        sqlite3_close(database);
        database = nullptr;
    }
}

bool DashboardServer::initialize() {
    if (!initialize_database()) {
        return false;
    }
    miner_address = read_wallet_address();
    if (!load_saved_peers()) {
        return false;
    }
    if (!load_pending_transactions()) {
        return false;
    }
    network_node.start(8333);
    return true;
}

bool DashboardServer::initialize_database() {
    std::filesystem::create_directories("dashboard/data");
    if (sqlite3_open("dashboard/data/network.sqlite", &database) != SQLITE_OK) {
        std::cerr << "SQLite open error: " << sqlite3_errmsg(database) << std::endl;
        return false;
    }

    const char* schema =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA foreign_keys = ON;"
        "CREATE TABLE IF NOT EXISTS blocks ("
        "height INTEGER PRIMARY KEY, hash TEXT NOT NULL UNIQUE, previous_hash TEXT NOT NULL,"
        "timestamp INTEGER NOT NULL, nonce INTEGER NOT NULL, difficulty INTEGER NOT NULL,"
        "miner_address TEXT NOT NULL, reward INTEGER NOT NULL, fees INTEGER NOT NULL DEFAULT 0, tx_count INTEGER NOT NULL,"
        "header_version INTEGER NOT NULL DEFAULT 1);"
        "CREATE TABLE IF NOT EXISTS transactions ("
        "txid TEXT PRIMARY KEY, block_height INTEGER NOT NULL REFERENCES blocks(height),"
        "kind TEXT NOT NULL, from_address TEXT NOT NULL, to_address TEXT NOT NULL,"
        "amount INTEGER NOT NULL, timestamp INTEGER NOT NULL, serialized_data TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS transactions_by_height ON transactions(block_height);"
        "CREATE TABLE IF NOT EXISTS pending_transactions ("
        "txid TEXT PRIMARY KEY, serialized_data TEXT NOT NULL, added_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS balances ("
        "address TEXT PRIMARY KEY, balance INTEGER NOT NULL DEFAULT 0,"
        "total_received INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS peers ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, host TEXT NOT NULL, port INTEGER NOT NULL,"
        "status TEXT NOT NULL CHECK(status IN ('pending', 'connected', 'offline')),"
        "latency_ms INTEGER, last_seen INTEGER, added_at INTEGER NOT NULL, UNIQUE(host, port));";
    if (!execute_sql(database, schema)) {
        return false;
    }
    if (!table_has_column(database, "blocks", "header_version") &&
        !execute_sql(database, "ALTER TABLE blocks ADD COLUMN header_version INTEGER NOT NULL DEFAULT 1")) {
        return false;
    }
    if (!table_has_column(database, "blocks", "fees") &&
        !execute_sql(database, "ALTER TABLE blocks ADD COLUMN fees INTEGER NOT NULL DEFAULT 0")) {
        return false;
    }
    if (!table_has_column(database, "transactions", "serialized_data") &&
        !execute_sql(database, "ALTER TABLE transactions ADD COLUMN serialized_data TEXT NOT NULL DEFAULT ''")) {
        return false;
    }

    BlockOfBlockChain genesis_block = BlockOfBlockChain::create_genesis_block();
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM blocks", -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_step(statement);
    const int block_count = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);

    if (block_count == 0) {
        if (sqlite3_prepare_v2(database,
            "INSERT INTO blocks(height, hash, previous_hash, timestamp, nonce, difficulty, miner_address, reward, tx_count)"
            " VALUES(0, ?, ?, 0, 0, 0, 'genesis', 0, 0)", -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(statement, 1, genesis_block.get_hash().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, zero_hash.c_str(), -1, SQLITE_TRANSIENT);
        const bool inserted = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        if (!inserted) return false;
    } else {
        if (sqlite3_prepare_v2(database,
            "SELECT hash, MAX(height) FROM blocks", -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_step(statement);
        const char* saved_hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const std::string database_genesis_hash = saved_hash ? saved_hash : "";
        const int latest_height = sqlite3_column_int(statement, 1);
        sqlite3_finalize(statement);
        if (latest_height == 0 && database_genesis_hash != genesis_block.get_hash()) {
            if (sqlite3_prepare_v2(database,
                "UPDATE blocks SET hash = ?, previous_hash = ?, timestamp = 0, nonce = 0, difficulty = 0,"
                " miner_address = 'genesis', reward = 0, tx_count = 0 WHERE height = 0",
                -1, &statement, nullptr) != SQLITE_OK) {
                return false;
            }
            sqlite3_bind_text(statement, 1, genesis_block.get_hash().c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, zero_hash.c_str(), -1, SQLITE_TRANSIENT);
            const bool updated = sqlite3_step(statement) == SQLITE_DONE;
            sqlite3_finalize(statement);
            if (!updated) return false;
        }
    }

    return load_blockchain();
}

bool DashboardServer::load_blockchain() {
    sqlite3_stmt* statement = nullptr;
    const char* query = "SELECT height, hash, previous_hash, timestamp, nonce, difficulty, miner_address, reward, header_version, fees"
                        " FROM blocks WHERE height > 0 ORDER BY height";
    if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    bool valid = true;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* saved_hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        const char* previous_hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        const char* address = reinterpret_cast<const char*>(sqlite3_column_text(statement, 6));
        const uint64_t height = static_cast<uint64_t>(sqlite3_column_int64(statement, 0));
        const int64_t stored_timestamp = sqlite3_column_int64(statement, 3);
        const uint64_t stored_nonce = static_cast<uint64_t>(sqlite3_column_int64(statement, 4));
        const uint64_t stored_reward = static_cast<uint64_t>(sqlite3_column_int64(statement, 7));
        const uint32_t stored_difficulty = static_cast<uint32_t>(sqlite3_column_int(statement, 5));
        const uint32_t stored_header_version = static_cast<uint32_t>(sqlite3_column_int(statement, 8));
        const uint64_t stored_fees = static_cast<uint64_t>(sqlite3_column_int64(statement, 9));
        const std::string previous = previous_hash ? previous_hash : "";
        const std::string miner = address ? address : "";
        BlockOfBlockChain block(height, previous, stored_timestamp, stored_nonce,
                                miner, stored_reward, stored_difficulty, stored_header_version, stored_fees);

        if (stored_header_version >= 2) {
            sqlite3_stmt* transaction_statement = nullptr;
            if (sqlite3_prepare_v2(database,
                "SELECT serialized_data FROM transactions WHERE block_height = ? AND kind = 'transfer' ORDER BY rowid",
                -1, &transaction_statement, nullptr) != SQLITE_OK) {
                valid = false;
                break;
            }
            sqlite3_bind_int64(transaction_statement, 1, static_cast<sqlite3_int64>(height));
            while (sqlite3_step(transaction_statement) == SQLITE_ROW) {
                const char* serialized = reinterpret_cast<const char*>(sqlite3_column_text(transaction_statement, 0));
                Transaction transaction;
                if (!serialized || !Transaction::deserialize(serialized, transaction) ||
                    !block.add_transaction(transaction)) {
                    valid = false;
                    break;
                }
            }
            sqlite3_finalize(transaction_statement);
            if (!valid) break;
        }

        if (!saved_hash) {
            valid = false;
            break;
        }
        if (block.get_hash() != saved_hash) {
            bool timestamp_restored = false;
            for (int64_t offset = 1; offset <= 600 && stored_timestamp > offset; ++offset) {
                BlockOfBlockChain candidate(height, previous, stored_timestamp - offset,
                                            stored_nonce, miner, stored_reward, stored_difficulty,
                                            stored_header_version, stored_fees);
                if (candidate.get_hash() == saved_hash) {
                    block = candidate;
                    timestamp_restored = true;
                    break;
                }
            }
            if (!timestamp_restored) {
                valid = false;
                break;
            }
        }
        if (!blockchain.add_block(block)) {
            valid = false;
            break;
        }
    }
    sqlite3_finalize(statement);
    return valid;
}

bool DashboardServer::load_saved_peers() {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT host, port FROM peers", -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* host = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        if (host) {
            network_node.add_peer(host, static_cast<uint16_t>(sqlite3_column_int(statement, 1)));
        }
    }
    sqlite3_finalize(statement);
    return true;
}

bool DashboardServer::load_pending_transactions() {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT serialized_data FROM pending_transactions",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char* serialized = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        Transaction transaction;
        if (serialized && Transaction::deserialize(serialized, transaction)) {
            network_node.get_mempool().add_transaction(transaction);
        }
    }
    sqlite3_finalize(statement);
    return true;
}

bool DashboardServer::run() {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return false;
    }
    int reuse_address = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(server_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server_socket, 16) < 0) {
        std::cerr << "Could not listen on 127.0.0.1:" << port << ": " << std::strerror(errno) << std::endl;
        close(server_socket);
        server_socket = -1;
        return false;
    }

    server_running.store(true);
    server_loop();
    return true;
}

void DashboardServer::server_loop() {
    pollfd listener{};
    listener.fd = server_socket;
    listener.events = POLLIN;
    while (server_running.load()) {
        const int ready = poll(&listener, 1, 500);
        if (ready <= 0 || !(listener.revents & POLLIN)) {
            continue;
        }
        const int client_socket = accept(server_socket, nullptr, nullptr);
        if (client_socket < 0) {
            if (!server_running.load()) break;
            continue;
        }
        handle_client(client_socket);
        close(client_socket);
    }
}

void DashboardServer::stop() {
    server_running.store(false);
    if (server_socket >= 0) {
        shutdown(server_socket, SHUT_RDWR);
        close(server_socket);
        server_socket = -1;
    }
    stop_mining();
}

bool DashboardServer::read_request(int client_socket, HttpRequest& request) {
    std::string data;
    std::array<char, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t received = recv(client_socket, buffer.data(), buffer.size(), 0);
        if (received <= 0 || data.size() + static_cast<std::size_t>(received) > max_request_size) {
            return false;
        }
        data.append(buffer.data(), static_cast<std::size_t>(received));
    }

    std::istringstream headers(data.substr(0, header_end));
    std::string request_line;
    if (!std::getline(headers, request_line)) return false;
    std::istringstream request_parts(request_line);
    std::string url;
    std::string protocol;
    request_parts >> request.method >> url >> protocol;
    const std::size_t query_pos = url.find('?');
    request.path = url.substr(0, query_pos);
    if (query_pos != std::string::npos) request.query = url.substr(query_pos + 1);

    std::size_t content_length = 0;
    std::string header;
    while (std::getline(headers, header)) {
        std::transform(header.begin(), header.end(), header.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (header.rfind("content-length:", 0) == 0) {
            try {
                content_length = static_cast<std::size_t>(std::stoul(header.substr(15)));
            } catch (const std::exception&) {
                return false;
            }
        }
    }
    if (content_length > max_request_size) return false;

    const std::size_t body_start = header_end + 4;
    while (data.size() - body_start < content_length) {
        const ssize_t received = recv(client_socket, buffer.data(), buffer.size(), 0);
        if (received <= 0 || data.size() + static_cast<std::size_t>(received) > max_request_size) {
            return false;
        }
        data.append(buffer.data(), static_cast<std::size_t>(received));
    }
    request.body = data.substr(body_start, content_length);
    return !request.method.empty() && !request.path.empty();
}

void DashboardServer::handle_client(int client_socket) {
    HttpRequest request;
    if (!read_request(client_socket, request)) {
        send_response(client_socket, 400, "application/json", "{\"error\":\"Bad request\"}");
        return;
    }
    handle_request(client_socket, request);
}

void DashboardServer::handle_request(int client_socket, const HttpRequest& request) {
    if (request.method == "GET" && request.path == "/api/state") {
        send_response(client_socket, 200, "application/json; charset=utf-8", build_state_json());
        return;
    }
    if (request.method == "GET" && request.path == "/api/wallets") {
        send_response(client_socket, 200, "application/json; charset=utf-8", build_wallets_json());
        return;
    }
    if (request.method == "POST" && request.path == "/api/wallets") {
        if (!create_wallet()) {
            send_response(client_socket, 500, "application/json", "{\"error\":\"Could not create wallet\"}");
            return;
        }
        send_response(client_socket, 201, "application/json; charset=utf-8", build_wallets_json());
        return;
    }
    if (request.method == "POST" && request.path == "/api/transactions") {
        submit_wallet_transaction(client_socket, request);
        return;
    }
    if (request.method == "GET" && request.path == "/api/transactions") {
        unsigned int limit = 50;
        const std::size_t limit_pos = request.query.find("limit=");
        if (limit_pos != std::string::npos) {
            try {
                limit = static_cast<unsigned int>(std::stoul(request.query.substr(limit_pos + 6)));
            } catch (const std::exception&) {
                limit = 50;
            }
        }
        limit = std::max(1u, std::min(200u, limit));
        send_response(client_socket, 200, "application/json; charset=utf-8", build_transactions_json(limit));
        return;
    }
    if (request.method == "POST" && request.path == "/api/mining") {
        bool valid_json = false;
        {
            std::lock_guard<std::mutex> lock(database_mutex);
            sqlite3_stmt* statement = nullptr;
            if (sqlite3_prepare_v2(database, "SELECT json_valid(?)", -1, &statement, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(statement, 1, request.body.c_str(), -1, SQLITE_TRANSIENT);
                valid_json = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1;
            }
            sqlite3_finalize(statement);
        }
        if (!valid_json) {
            send_response(client_socket, 400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        const std::string address = get_json_string(request.body, "address");
        const bool enabled = get_json_boolean(request.body, "enabled");
        if (!address.empty() && !valid_address(address)) {
            send_response(client_socket, 400, "application/json", "{\"error\":\"Invalid NTR address\"}");
            return;
        }
        if (enabled) {
            std::string selected_address = address;
            if (selected_address.empty()) {
                std::lock_guard<std::mutex> lock(mining_mutex);
                selected_address = miner_address;
            }
            if (!start_mining(selected_address)) {
                send_response(client_socket, 500, "application/json", "{\"error\":\"Could not start miner\"}");
                return;
            }
        } else {
            if (!address.empty()) {
                std::lock_guard<std::mutex> lock(mining_mutex);
                miner_address = address;
            }
            stop_mining();
        }
        send_response(client_socket, 200, "application/json; charset=utf-8", build_state_json());
        return;
    }
    if (request.method == "POST" && request.path == "/api/peers") {
        const std::string peer_host = get_json_string(request.body, "host");
        const int64_t peer_port = get_json_integer(request.body, "port");
        if (peer_host.empty() || peer_host.size() > 253 || peer_port < 1 || peer_port > 65535) {
            send_response(client_socket, 400, "application/json", "{\"error\":\"Invalid peer host or port\"}");
            return;
        }
        if (!add_peer(peer_host, static_cast<uint16_t>(peer_port))) {
            send_response(client_socket, 500, "application/json", "{\"error\":\"Could not save peer\"}");
            return;
        }
        send_response(client_socket, 201, "application/json; charset=utf-8", build_state_json());
        return;
    }
    if (request.method == "DELETE" && request.path.rfind("/api/peers/", 0) == 0) {
        try {
            const int peer_id = std::stoi(request.path.substr(11));
            if (!delete_peer(peer_id)) {
                send_response(client_socket, 404, "application/json", "{\"error\":\"Peer not found\"}");
                return;
            }
            send_response(client_socket, 200, "application/json", "{\"ok\":true}");
        } catch (const std::exception&) {
            send_response(client_socket, 400, "application/json", "{\"error\":\"Invalid peer id\"}");
        }
        return;
    }

    if (request.method != "GET" && request.method != "HEAD") {
        send_response(client_socket, 405, "application/json", "{\"error\":\"Method not allowed\"}");
        return;
    }

    std::string file_path;
    std::string content_type;
    if (request.path == "/" || request.path == "/index.html") {
        file_path = "dashboard/public/index.html";
        content_type = "text/html; charset=utf-8";
    } else if (request.path == "/styles.css") {
        file_path = "dashboard/public/styles.css";
        content_type = "text/css; charset=utf-8";
    } else if (request.path == "/app.js") {
        file_path = "dashboard/public/app.js";
        content_type = "text/javascript; charset=utf-8";
    } else {
        send_response(client_socket, 404, "text/plain; charset=utf-8", "Not found");
        return;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        send_response(client_socket, 404, "text/plain; charset=utf-8", "Static file not found");
        return;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    send_response(client_socket, 200, content_type, request.method == "HEAD" ? "" : contents.str());
}

void DashboardServer::send_response(int client_socket, int status,
                                    const std::string& content_type,
                                    const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << status_text(status) << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n" << body;
    send_all(client_socket, response.str());
}

std::string DashboardServer::build_state_json() {
    std::scoped_lock lock(database_mutex, blockchain_mutex, mining_mutex);
    const BlockOfBlockChain& latest_block = blockchain.get_latest_block();
    std::ostringstream json;
    sqlite3_stmt* statement = nullptr;

    int64_t block_count = 0;
    int64_t minted = 0;
    int64_t miner_count = 0;
    int64_t transaction_count = 0;
    if (sqlite3_prepare_v2(database,
        "SELECT COUNT(*), COALESCE(SUM(reward), 0),"
        " COUNT(DISTINCT CASE WHEN reward > 0 THEN miner_address END) FROM blocks",
        -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        block_count = sqlite3_column_int64(statement, 0);
        minted = sqlite3_column_int64(statement, 1);
        miner_count = sqlite3_column_int64(statement, 2);
    }
    sqlite3_finalize(statement);
    if (sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM transactions", -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        transaction_count = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);

    json << "{\"chain\":{\"height\":" << latest_block.get_index()
         << ",\"hash\":\"" << json_escape(latest_block.get_hash())
         << "\",\"blocks\":" << block_count << ",\"minted\":" << minted
         << ",\"miners\":" << miner_count << ",\"transactions\":" << transaction_count
         << ",\"difficulty\":" << blockchain.get_next_difficulty() << "},\"mining\":{\"enabled\":"
         << (mining_enabled.load() ? "true" : "false")
         << ",\"minerAddress\":\"" << json_escape(miner_address)
         << "\",\"nonce\":" << nonce << ",\"hashes\":" << hashes
         << ",\"hashRate\":" << hash_rate << ",\"candidateHash\":\""
         << json_escape(candidate_hash) << "\",\"progress\":"
         << std::min(99.0, static_cast<double>(hashes) / 16.0 / 16.0 / 16.0 / 16.0 / 16.0 * 100.0)
         << ",\"startedAt\":null,\"lastReward\":";

    if (sqlite3_prepare_v2(database,
        "SELECT blocks.height, blocks.miner_address, COALESCE(transactions.amount, blocks.reward), blocks.timestamp"
        " FROM blocks LEFT JOIN transactions ON transactions.block_height = blocks.height"
        " AND transactions.kind = 'coinbase' WHERE blocks.reward > 0 ORDER BY blocks.height DESC LIMIT 1",
        -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        const char* reward_address = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        json << "{\"height\":" << sqlite3_column_int64(statement, 0)
             << ",\"address\":\"" << json_escape(reward_address ? reward_address : "")
             << "\",\"amount\":" << sqlite3_column_int64(statement, 2)
             << ",\"timestamp\":" << sqlite3_column_int64(statement, 3) << "}";
    } else {
        json << "null";
    }
    sqlite3_finalize(statement);
    json << ",\"samples\":[";
    for (std::size_t i = 0; i < hash_rate_samples.size(); ++i) {
        if (i) json << ',';
        json << hash_rate_samples[i];
    }
    json << "]},\"peers\":{\"counts\":{";

    int connected = 0;
    int pending = 0;
    int offline = 0;
    if (sqlite3_prepare_v2(database,
        "SELECT SUM(status = 'connected'), SUM(status = 'pending'), SUM(status = 'offline') FROM peers",
        -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) {
        connected = sqlite3_column_int(statement, 0);
        pending = sqlite3_column_int(statement, 1);
        offline = sqlite3_column_int(statement, 2);
    }
    sqlite3_finalize(statement);
    json << "\"connected\":" << connected << ",\"pending\":" << pending
         << ",\"offline\":" << offline << "},\"items\":[";
    if (sqlite3_prepare_v2(database,
        "SELECT id, host, port, status, latency_ms, last_seen, added_at FROM peers ORDER BY added_at DESC",
        -1, &statement, nullptr) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            if (!first) json << ',';
            first = false;
            const char* peer_host = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
            const char* peer_status = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
            json << "{\"id\":" << sqlite3_column_int(statement, 0)
                 << ",\"host\":\"" << json_escape(peer_host ? peer_host : "")
                 << "\",\"port\":" << sqlite3_column_int(statement, 2)
                 << ",\"status\":\"" << json_escape(peer_status ? peer_status : "offline")
                 << "\",\"latencyMs\":";
            if (sqlite3_column_type(statement, 4) == SQLITE_NULL) json << "null";
            else json << sqlite3_column_int(statement, 4);
            json << ",\"lastSeen\":";
            if (sqlite3_column_type(statement, 5) == SQLITE_NULL) json << "null";
            else json << sqlite3_column_int64(statement, 5);
            json << ",\"addedAt\":" << sqlite3_column_int64(statement, 6) << "}";
        }
    }
    sqlite3_finalize(statement);

    json << "]},\"blocks\":[";
    if (sqlite3_prepare_v2(database,
        "SELECT height, hash, previous_hash, timestamp, nonce, difficulty, miner_address, reward, tx_count"
        " FROM blocks ORDER BY height DESC LIMIT 12", -1, &statement, nullptr) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            if (!first) json << ',';
            first = false;
            const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
            const char* previous_hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
            const char* address = reinterpret_cast<const char*>(sqlite3_column_text(statement, 6));
            json << "{\"height\":" << sqlite3_column_int64(statement, 0)
                 << ",\"hash\":\"" << json_escape(hash ? hash : "")
                 << "\",\"previousHash\":\"" << json_escape(previous_hash ? previous_hash : "")
                 << "\",\"timestamp\":" << sqlite3_column_int64(statement, 3)
                 << ",\"nonce\":" << sqlite3_column_int64(statement, 4)
                 << ",\"difficulty\":" << sqlite3_column_int(statement, 5)
                 << ",\"minerAddress\":\"" << json_escape(address ? address : "")
                 << "\",\"reward\":" << sqlite3_column_int64(statement, 7)
                 << ",\"transactionCount\":" << sqlite3_column_int(statement, 8) << "}";
        }
    }
    sqlite3_finalize(statement);

    json << "],\"balances\":[";
    if (sqlite3_prepare_v2(database,
        "SELECT address, balance, total_received FROM balances ORDER BY balance DESC, address LIMIT 8",
        -1, &statement, nullptr) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            if (!first) json << ',';
            first = false;
            const char* address = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
            json << "{\"address\":\"" << json_escape(address ? address : "")
                 << "\",\"balance\":" << sqlite3_column_int64(statement, 1)
                 << ",\"totalReceived\":" << sqlite3_column_int64(statement, 2) << "}";
        }
    }
    sqlite3_finalize(statement);
    json << "],\"mempool\":" << build_mempool_json() << "}";
    return json.str();
}

std::string DashboardServer::build_transactions_json(unsigned int limit) {
    std::lock_guard<std::mutex> lock(database_mutex);
    std::ostringstream json;
    sqlite3_stmt* statement = nullptr;
    json << "{\"transactions\":[";
    if (sqlite3_prepare_v2(database,
        "SELECT txid, block_height, kind, from_address, to_address, amount, timestamp"
        " FROM transactions ORDER BY block_height DESC LIMIT ?", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(statement, 1, static_cast<int>(limit));
        bool first = true;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            if (!first) json << ',';
            first = false;
            const char* txid = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
            const char* kind = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
            const char* from_address = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
            const char* to_address = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
            json << "{\"txid\":\"" << json_escape(txid ? txid : "")
                 << "\",\"height\":" << sqlite3_column_int64(statement, 1)
                 << ",\"kind\":\"" << json_escape(kind ? kind : "")
                 << "\",\"fromAddress\":\"" << json_escape(from_address ? from_address : "")
                 << "\",\"toAddress\":\"" << json_escape(to_address ? to_address : "")
                 << "\",\"amount\":" << sqlite3_column_int64(statement, 5)
                 << ",\"timestamp\":" << sqlite3_column_int64(statement, 6) << "}";
        }
    }
    sqlite3_finalize(statement);
    json << "]}";
    return json.str();
}

std::string DashboardServer::build_wallets_json() {
    std::vector<std::string> wallet_files;
    if (std::filesystem::exists("wallets") && std::filesystem::is_directory("wallets")) {
        for (const auto& entry : std::filesystem::directory_iterator("wallets")) {
            const std::string file_name = entry.path().filename().string();
            if (entry.is_regular_file() && valid_wallet_file(file_name)) {
                wallet_files.push_back(file_name);
            }
        }
    }
    std::sort(wallet_files.begin(), wallet_files.end(), [](const std::string& first, const std::string& second) {
        return std::stoul(first.substr(6, first.size() - 10)) <
               std::stoul(second.substr(6, second.size() - 10));
    });

    std::ostringstream json;
    json << "{\"wallets\":[";
    bool first_wallet = true;
    for (const std::string& wallet_file : wallet_files) {
        std::string address;
        if (!wallet_opening("wallets/" + wallet_file, nullptr, nullptr, &address)) continue;
        int64_t balance = 0;
        {
            std::lock_guard<std::mutex> lock(database_mutex);
            sqlite3_stmt* statement = nullptr;
            if (sqlite3_prepare_v2(database, "SELECT balance FROM balances WHERE address = ?",
                                   -1, &statement, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(statement, 1, address.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(statement) == SQLITE_ROW) balance = sqlite3_column_int64(statement, 0);
            }
            sqlite3_finalize(statement);
        }
        for (const Transaction& transaction : network_node.get_mempool().get_transactions()) {
            if (transaction.get_sender() == address) {
                const uint64_t spend = transaction.get_amount() + transaction.get_fee();
                balance = spend >= static_cast<uint64_t>(balance)
                    ? 0 : balance - static_cast<int64_t>(spend);
            }
        }
        if (!first_wallet) json << ',';
        first_wallet = false;
        json << "{\"file\":\"" << json_escape(wallet_file)
             << "\",\"address\":\"" << json_escape(address)
             << "\",\"balance\":" << balance << "}";
    }
    json << "]}";
    return json.str();
}

std::string DashboardServer::build_mempool_json() {
    const std::vector<Transaction> transactions = network_node.get_mempool().get_transactions();
    std::ostringstream json;
    json << '[';
    for (std::size_t i = 0; i < transactions.size(); ++i) {
        if (i) json << ',';
        json << "{\"txid\":\"" << json_escape(transactions[i].getTxID())
             << "\",\"fromAddress\":\"" << json_escape(transactions[i].get_sender())
             << "\",\"toAddress\":\"" << json_escape(transactions[i].get_recipient())
             << "\",\"amount\":" << transactions[i].get_amount()
             << ",\"nonce\":" << transactions[i].get_nonce()
             << ",\"status\":\"pending\"}";
    }
    json << ']';
    return json.str();
}

bool DashboardServer::valid_wallet_file(const std::string& wallet_file) const {
    if (wallet_file.size() < 10 || wallet_file.rfind("wallet", 0) != 0 ||
        wallet_file.substr(wallet_file.size() - 4) != ".txt") {
        return false;
    }
    const std::string number = wallet_file.substr(6, wallet_file.size() - 10);
    return !number.empty() && std::all_of(number.begin(), number.end(), [](unsigned char character) {
        return std::isdigit(character) != 0;
    });
}

bool DashboardServer::create_wallet() {
    Wallet wallet;
    if (wallet.get_address().empty()) return false;
    for (const auto& entry : std::filesystem::directory_iterator("wallets")) {
        std::string address;
        if (entry.is_regular_file() && valid_wallet_file(entry.path().filename().string()) &&
            wallet_opening(entry.path().string(), nullptr, nullptr, &address) &&
            address == wallet.get_address()) {
            return true;
        }
    }
    return false;
}

void DashboardServer::submit_wallet_transaction(int client_socket, const HttpRequest& request) {
    const std::string wallet_file = get_json_string(request.body, "wallet");
    const std::string recipient = get_json_string(request.body, "recipient");
    const std::string amount_text = get_json_string(request.body, "amount");
    uint64_t amount = 0;
    if (!valid_wallet_file(wallet_file) || !valid_address(recipient) ||
        !parse_ntr_amount(amount_text, amount)) {
        send_response(client_socket, 400, "application/json", "{\"error\":\"Invalid wallet, recipient or amount\"}");
        return;
    }

    std::string private_key;
    std::string sender;
    if (!wallet_opening("wallets/" + wallet_file, &private_key, nullptr, &sender)) {
        send_response(client_socket, 404, "application/json", "{\"error\":\"Wallet file not found\"}");
        return;
    }

    uint64_t transaction_nonce = 0;
    uint64_t pending_spend = 0;
    const std::vector<Transaction> pending_transactions = network_node.get_mempool().get_transactions();
    for (const Transaction& pending : pending_transactions) {
        if (pending.get_sender() == sender) {
            transaction_nonce = std::max(transaction_nonce, pending.get_nonce() + 1);
            if (pending.get_amount() > std::numeric_limits<uint64_t>::max() - pending.get_fee() ||
                pending.get_amount() + pending.get_fee() > std::numeric_limits<uint64_t>::max() - pending_spend) {
                send_response(client_socket, 400, "application/json", "{\"error\":\"Pending amount overflow\"}");
                return;
            }
            pending_spend += pending.get_amount() + pending.get_fee();
        }
    }

    uint64_t confirmed_balance = 0;
    {
        std::lock_guard<std::mutex> lock(database_mutex);
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT balance FROM balances WHERE address = ?",
                               -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement) == SQLITE_ROW) {
                confirmed_balance = static_cast<uint64_t>(sqlite3_column_int64(statement, 0));
            }
        }
        sqlite3_finalize(statement);

        if (sqlite3_prepare_v2(database,
            "SELECT serialized_data FROM transactions WHERE from_address = ? AND kind = 'transfer'",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(statement, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const char* serialized = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
                Transaction confirmed;
                if (serialized && Transaction::deserialize(serialized, confirmed)) {
                    transaction_nonce = std::max(transaction_nonce, confirmed.get_nonce() + 1);
                }
            }
        }
        sqlite3_finalize(statement);
    }

    Transaction transaction(sender, recipient, amount, transaction_nonce, current_timestamp());
    if (amount > std::numeric_limits<uint64_t>::max() - transaction.get_fee() ||
        pending_spend > confirmed_balance ||
        amount + transaction.get_fee() > confirmed_balance - pending_spend) {
        send_response(client_socket, 400, "application/json", "{\"error\":\"Insufficient available balance\"}");
        return;
    }
    if (!transaction.generate_signature(private_key)) {
        send_response(client_socket, 400, "application/json", "{\"error\":\"Could not sign transaction\"}");
        return;
    }

    const SubmissionResult result = network_node.submit_transaction(transaction);
    if (result == SubmissionResult::RejectedInvalid) {
        send_response(client_socket, 400, "application/json", "{\"error\":\"Transaction signature is invalid\"}");
        return;
    }
    if (result == SubmissionResult::AlreadyKnown) {
        send_response(client_socket, 409, "application/json", "{\"error\":\"Transaction is already in mempool\"}");
        return;
    }

    bool pending_saved = false;
    {
        std::lock_guard<std::mutex> lock(database_mutex);
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database,
            "INSERT INTO pending_transactions(txid, serialized_data, added_at) VALUES(?, ?, ?)",
            -1, &statement, nullptr) == SQLITE_OK) {
            const std::string serialized = transaction.serialize();
            sqlite3_bind_text(statement, 1, transaction.getTxID().c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 2, serialized.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(statement, 3, current_timestamp() * 1000);
            pending_saved = sqlite3_step(statement) == SQLITE_DONE;
        }
        sqlite3_finalize(statement);
    }
    if (!pending_saved) {
        network_node.get_mempool().remove_transaction(transaction.getTxID());
        send_response(client_socket, 500, "application/json", "{\"error\":\"Could not persist pending transaction\"}");
        return;
    }

    const bool broadcast = result == SubmissionResult::AcceptedAndBroadcast;
    std::ostringstream response;
    response << "{\"txid\":\"" << json_escape(transaction.getTxID())
             << "\",\"status\":\"pending\",\"broadcast\":"
             << (broadcast ? "true" : "false") << "}";
    send_response(client_socket, 201, "application/json; charset=utf-8", response.str());
}

std::string DashboardServer::get_json_string(const std::string& body, const std::string& key) {
    std::lock_guard<std::mutex> lock(database_mutex);
    sqlite3_stmt* statement = nullptr;
    const std::string path = "$." + key;
    std::string value;
    if (sqlite3_prepare_v2(database, "SELECT json_extract(?, ?)", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_TEXT) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
            if (result) value = result;
        }
    }
    sqlite3_finalize(statement);
    return value;
}

int64_t DashboardServer::get_json_integer(const std::string& body, const std::string& key) {
    std::lock_guard<std::mutex> lock(database_mutex);
    sqlite3_stmt* statement = nullptr;
    const std::string path = "$." + key;
    int64_t value = 0;
    if (sqlite3_prepare_v2(database, "SELECT json_extract(?, ?)", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return value;
}

bool DashboardServer::get_json_boolean(const std::string& body, const std::string& key) {
    std::lock_guard<std::mutex> lock(database_mutex);
    sqlite3_stmt* statement = nullptr;
    const std::string path = "$." + key;
    bool value = false;
    if (sqlite3_prepare_v2(database, "SELECT json_extract(?, ?)", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int(statement, 0) == 1;
    }
    sqlite3_finalize(statement);
    return value;
}

bool DashboardServer::valid_address(const std::string& address) const {
    if (address.size() < 12 || address.size() > 90 || address.rfind("ntr1", 0) != 0) {
        return false;
    }
    for (unsigned char character : address) {
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return true;
}

bool DashboardServer::start_mining(const std::string& address) {
    if (!valid_address(address)) return false;
    stop_mining();
    {
        std::lock_guard<std::mutex> lock(mining_mutex);
        miner_address = address;
        nonce = 0;
        hashes = 0;
        hash_rate = 0;
        candidate_hash = "searching";
    }
    mining_enabled.store(true);
    mining_thread = std::thread(&DashboardServer::mining_loop, this);
    return true;
}

void DashboardServer::stop_mining() {
    mining_enabled.store(false);
    if (mining_thread.joinable() && mining_thread.get_id() != std::this_thread::get_id()) {
        mining_thread.join();
    }
    std::lock_guard<std::mutex> lock(mining_mutex);
    hash_rate = 0;
}

void DashboardServer::mining_loop() {
    while (mining_enabled.load()) {
        BlockOfBlockChain previous_block;
        uint32_t block_difficulty = 0;
        {
            std::lock_guard<std::mutex> lock(blockchain_mutex);
            previous_block = blockchain.get_latest_block();
            block_difficulty = blockchain.get_next_difficulty();
        }
        std::string payout_address;
        {
            std::lock_guard<std::mutex> lock(mining_mutex);
            payout_address = miner_address;
            hashes = 0;
            nonce = 0;
            hash_rate = 0;
            candidate_hash = "searching";
        }

        BlockOfBlockChain block(previous_block.get_index() + 1,
                                previous_block.get_hash(), current_timestamp());
        std::vector<Transaction> block_transactions;
        std::unordered_map<std::string, uint64_t> available_balances;
        uint64_t total_fees = 0;
        const std::vector<Transaction> pending_transactions = network_node.get_mempool().get_transactions();
        for (const Transaction& transaction : pending_transactions) {
            if (!valid_address(transaction.get_sender()) || !valid_address(transaction.get_recipient()) ||
                transaction.get_amount() > std::numeric_limits<uint64_t>::max() - transaction.get_fee() ||
                transaction.get_fee() > std::numeric_limits<uint64_t>::max() - total_fees) {
                continue;
            }
            const uint64_t spend = transaction.get_amount() + transaction.get_fee();
            auto balance = available_balances.find(transaction.get_sender());
            if (balance == available_balances.end()) {
                uint64_t confirmed_balance = 0;
                {
                    std::lock_guard<std::mutex> lock(database_mutex);
                    sqlite3_stmt* statement = nullptr;
                    if (sqlite3_prepare_v2(database, "SELECT balance FROM balances WHERE address = ?",
                                           -1, &statement, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(statement, 1, transaction.get_sender().c_str(), -1, SQLITE_TRANSIENT);
                        if (sqlite3_step(statement) == SQLITE_ROW) {
                            confirmed_balance = static_cast<uint64_t>(sqlite3_column_int64(statement, 0));
                        }
                    }
                    sqlite3_finalize(statement);
                }
                balance = available_balances.emplace(transaction.get_sender(), confirmed_balance).first;
            }
            if (balance->second < spend) continue;
            balance->second -= spend;
            if (block.add_transaction(transaction)) {
                block_transactions.push_back(transaction);
                total_fees += transaction.get_fee();
            } else {
                balance->second += spend;
            }
        }
        block.set_mining_data(payout_address, block_reward, block_difficulty, total_fees);
        const auto started = std::chrono::steady_clock::now();
        auto last_update = started;
        uint64_t current_nonce = 0;
        while (mining_enabled.load()) {
            for (int attempt = 0; attempt < 1800 && mining_enabled.load(); ++attempt) {
                block.set_nonce(current_nonce);
                if (block.has_valid_proof()) break;
                ++current_nonce;
            }
            const auto now = std::chrono::steady_clock::now();
            const uint64_t elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
            {
                std::lock_guard<std::mutex> lock(mining_mutex);
                nonce = current_nonce;
                hashes = current_nonce + 1;
                hash_rate = hashes * 1000 / std::max<uint64_t>(1, elapsed_ms);
                candidate_hash = block.get_hash();
                if (hashes % 100000 == 0 || now - last_update >= std::chrono::milliseconds(250)) {
                    last_update = now;
                }
            }
            if (block.has_valid_proof()) break;
            std::this_thread::yield();
        }
        if (!mining_enabled.load()) return;

        const int64_t block_timestamp = block.get_timestamp();
        const uint64_t coinbase_amount = block_reward + total_fees;
        const std::string transaction_id = sha256_text(
            std::to_string(block.get_index()) + ':' + block.get_hash() + ":coinbase:" +
            payout_address + ':' + std::to_string(coinbase_amount));
        bool block_saved = false;
        {
            std::scoped_lock lock(database_mutex, blockchain_mutex);
            if (execute_sql(database, "BEGIN IMMEDIATE")) {
                sqlite3_stmt* statement = nullptr;
                bool saved = sqlite3_prepare_v2(database,
                    "INSERT INTO blocks(height, hash, previous_hash, timestamp, nonce, difficulty, miner_address, reward, fees, tx_count, header_version)"
                    " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &statement, nullptr) == SQLITE_OK;
                if (saved) {
                    sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(block.get_index()));
                    sqlite3_bind_text(statement, 2, block.get_hash().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 3, block.get_previous_hash().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 4, block_timestamp);
                    sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(block.get_nonce()));
                    sqlite3_bind_int(statement, 6, static_cast<int>(block.get_difficulty()));
                    sqlite3_bind_text(statement, 7, payout_address.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 8, static_cast<sqlite3_int64>(block_reward));
                    sqlite3_bind_int64(statement, 9, static_cast<sqlite3_int64>(total_fees));
                    sqlite3_bind_int(statement, 10, static_cast<int>(block_transactions.size() + 1));
                    sqlite3_bind_int(statement, 11, static_cast<int>(block.get_header_version()));
                    saved = sqlite3_step(statement) == SQLITE_DONE;
                }
                sqlite3_finalize(statement);
                statement = nullptr;

                if (saved && sqlite3_prepare_v2(database,
                    "INSERT INTO transactions(txid, block_height, kind, from_address, to_address, amount, timestamp, serialized_data)"
                    " VALUES(?, ?, 'coinbase', 'coinbase', ?, ?, ?, '')", -1, &statement, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(statement, 1, transaction_id.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(block.get_index()));
                    sqlite3_bind_text(statement, 3, payout_address.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 4, static_cast<sqlite3_int64>(coinbase_amount));
                    sqlite3_bind_int64(statement, 5, block_timestamp);
                    saved = sqlite3_step(statement) == SQLITE_DONE;
                } else {
                    saved = false;
                }
                sqlite3_finalize(statement);
                statement = nullptr;

                for (const Transaction& transaction : block_transactions) {
                    if (!saved) break;
                    const uint64_t spend = transaction.get_amount() + transaction.get_fee();
                    if (sqlite3_prepare_v2(database,
                        "UPDATE balances SET balance = balance - ?, updated_at = ?"
                        " WHERE address = ? AND balance >= ?", -1, &statement, nullptr) != SQLITE_OK) {
                        saved = false;
                        break;
                    }
                    sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(spend));
                    sqlite3_bind_int64(statement, 2, block_timestamp);
                    sqlite3_bind_text(statement, 3, transaction.get_sender().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 4, static_cast<sqlite3_int64>(spend));
                    saved = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database) == 1;
                    sqlite3_finalize(statement);
                    statement = nullptr;
                    if (!saved) break;

                    if (sqlite3_prepare_v2(database,
                        "INSERT INTO balances(address, balance, total_received, updated_at) VALUES(?, ?, ?, ?)"
                        " ON CONFLICT(address) DO UPDATE SET balance = balance + excluded.balance,"
                        " total_received = total_received + excluded.total_received, updated_at = excluded.updated_at",
                        -1, &statement, nullptr) != SQLITE_OK) {
                        saved = false;
                        break;
                    }
                    sqlite3_bind_text(statement, 1, transaction.get_recipient().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(transaction.get_amount()));
                    sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(transaction.get_amount()));
                    sqlite3_bind_int64(statement, 4, block_timestamp);
                    saved = sqlite3_step(statement) == SQLITE_DONE;
                    sqlite3_finalize(statement);
                    statement = nullptr;
                    if (!saved) break;

                    const std::string serialized = transaction.serialize();
                    if (sqlite3_prepare_v2(database,
                        "INSERT INTO transactions(txid, block_height, kind, from_address, to_address, amount, timestamp, serialized_data)"
                        " VALUES(?, ?, 'transfer', ?, ?, ?, ?, ?)", -1, &statement, nullptr) != SQLITE_OK) {
                        saved = false;
                        break;
                    }
                    sqlite3_bind_text(statement, 1, transaction.getTxID().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(block.get_index()));
                    sqlite3_bind_text(statement, 3, transaction.get_sender().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(statement, 4, transaction.get_recipient().c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(transaction.get_amount()));
                    sqlite3_bind_int64(statement, 6, block_timestamp);
                    sqlite3_bind_text(statement, 7, serialized.c_str(), -1, SQLITE_TRANSIENT);
                    saved = sqlite3_step(statement) == SQLITE_DONE;
                    sqlite3_finalize(statement);
                    statement = nullptr;

                    if (saved && sqlite3_prepare_v2(database,
                        "DELETE FROM pending_transactions WHERE txid = ?", -1, &statement, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(statement, 1, transaction.getTxID().c_str(), -1, SQLITE_TRANSIENT);
                        saved = sqlite3_step(statement) == SQLITE_DONE;
                    } else {
                        saved = false;
                    }
                    sqlite3_finalize(statement);
                    statement = nullptr;
                }

                if (saved && sqlite3_prepare_v2(database,
                    "INSERT INTO balances(address, balance, total_received, updated_at) VALUES(?, ?, ?, ?)"
                    " ON CONFLICT(address) DO UPDATE SET balance = balance + excluded.balance,"
                    " total_received = total_received + excluded.total_received, updated_at = excluded.updated_at",
                    -1, &statement, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(statement, 1, payout_address.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(coinbase_amount));
                    sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(coinbase_amount));
                    sqlite3_bind_int64(statement, 4, block_timestamp);
                    saved = sqlite3_step(statement) == SQLITE_DONE;
                } else {
                    saved = false;
                }
                sqlite3_finalize(statement);

                if (saved && blockchain.add_block(block) && execute_sql(database, "COMMIT")) {
                    block_saved = true;
                } else {
                    execute_sql(database, "ROLLBACK");
                }
            }
        }
        if (!block_saved) {
            std::cerr << "Could not persist mined block" << std::endl;
            mining_enabled.store(false);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mining_mutex);
            hash_rate_samples.push_back(hash_rate);
            if (hash_rate_samples.size() > 48) hash_rate_samples.erase(hash_rate_samples.begin());
        }
        for (const Transaction& transaction : block_transactions) {
            network_node.get_mempool().remove_transaction(transaction.getTxID());
        }
    }
}

bool DashboardServer::add_peer(const std::string& host, uint16_t peer_port) {
    int peer_id = 0;
    {
        std::lock_guard<std::mutex> lock(database_mutex);
        sqlite3_stmt* statement = nullptr;
        const char* query = "INSERT INTO peers(host, port, status, added_at) VALUES(?, ?, 'pending', ?)"
                            " ON CONFLICT(host, port) DO UPDATE SET status = 'pending'";
        if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(statement, 1, host.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, peer_port);
        sqlite3_bind_int64(statement, 3, current_timestamp() * 1000);
        const bool inserted = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        if (!inserted) return false;

        if (sqlite3_prepare_v2(database, "SELECT id FROM peers WHERE host = ? AND port = ?",
                               -1, &statement, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(statement, 1, host.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, peer_port);
        if (sqlite3_step(statement) == SQLITE_ROW) peer_id = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
    }
    network_node.add_peer(host, peer_port);
    return peer_id > 0 && probe_peer(peer_id, host, peer_port);
}

bool DashboardServer::probe_peer(int peer_id, const std::string& host, uint16_t peer_port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(peer_port);
    const int lookup_result = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    bool connected = false;
    int latency = 0;
    if (lookup_result == 0) {
        for (addrinfo* address = addresses; address && !connected; address = address->ai_next) {
            const int socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket_fd < 0) continue;
            const int old_flags = fcntl(socket_fd, F_GETFL, 0);
            fcntl(socket_fd, F_SETFL, old_flags | O_NONBLOCK);
            const auto started = std::chrono::steady_clock::now();
            int result = connect(socket_fd, address->ai_addr, address->ai_addrlen);
            if (result < 0 && errno == EINPROGRESS) {
                pollfd pending_socket{};
                pending_socket.fd = socket_fd;
                pending_socket.events = POLLOUT;
                if (poll(&pending_socket, 1, 800) > 0) {
                    socklen_t error_size = sizeof(result);
                    getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &result, &error_size);
                }
            }
            if (result == 0) {
                connected = true;
                latency = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count());
            }
            close(socket_fd);
        }
        freeaddrinfo(addresses);
    }

    std::lock_guard<std::mutex> lock(database_mutex);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database,
        "UPDATE peers SET status = ?, latency_ms = ?, last_seen = CASE WHEN ? = 'connected' THEN ? ELSE last_seen END WHERE id = ?",
        -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const char* status = connected ? "connected" : "offline";
    sqlite3_bind_text(statement, 1, status, -1, SQLITE_STATIC);
    if (connected) sqlite3_bind_int(statement, 2, latency);
    else sqlite3_bind_null(statement, 2);
    sqlite3_bind_text(statement, 3, status, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 4, current_timestamp() * 1000);
    sqlite3_bind_int(statement, 5, peer_id);
    const bool updated = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return updated;
}

bool DashboardServer::delete_peer(int peer_id) {
    std::string host;
    uint16_t peer_port = 0;
    std::lock_guard<std::mutex> lock(database_mutex);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT host, port FROM peers WHERE id = ?",
                           -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(statement, 1, peer_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char* saved_host = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        host = saved_host ? saved_host : "";
        peer_port = static_cast<uint16_t>(sqlite3_column_int(statement, 1));
    }
    sqlite3_finalize(statement);
    if (host.empty()) return false;

    if (sqlite3_prepare_v2(database, "DELETE FROM peers WHERE id = ?", -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(statement, 1, peer_id);
    const bool deleted = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(database) > 0;
    sqlite3_finalize(statement);
    if (deleted) network_node.remove_peer(host, peer_port);
    return deleted;
}

std::string DashboardServer::json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (unsigned char character : value) {
        if (character == '"') escaped << "\\\"";
        else if (character == '\\') escaped << "\\\\";
        else if (character == '\b') escaped << "\\b";
        else if (character == '\f') escaped << "\\f";
        else if (character == '\n') escaped << "\\n";
        else if (character == '\r') escaped << "\\r";
        else if (character == '\t') escaped << "\\t";
        else if (character < 0x20) {
            escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(character) << std::dec;
        } else escaped << static_cast<char>(character);
    }
    return escaped.str();
}

std::string DashboardServer::status_text(int status) {
    if (status == 200) return "OK";
    if (status == 201) return "Created";
    if (status == 400) return "Bad Request";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    return "Internal Server Error";
}
