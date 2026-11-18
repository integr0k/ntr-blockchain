#include "wallet/wallet.hpp"
#include "crypto/helper.hpp"
#include "blockchain/blockchain.hpp"
#include "blockchain/transactions/transaction.hpp"
#include "network/node.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

uint64_t parse_ntr_amount(const std::string& value) {
    const std::size_t decimal_point = value.find('.');
    if (decimal_point != std::string::npos && value.find('.', decimal_point + 1) != std::string::npos) {
        throw std::invalid_argument("Amount format is incorrect.");
    }
    const std::string whole_text = value.substr(0, decimal_point);
    const std::string fraction_text = decimal_point == std::string::npos
        ? std::string() : value.substr(decimal_point + 1);
    if (whole_text.empty() || fraction_text.size() > 6 ||
        !std::all_of(whole_text.begin(), whole_text.end(), [](unsigned char ch) { return std::isdigit(ch); }) ||
        !std::all_of(fraction_text.begin(), fraction_text.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        throw std::invalid_argument("Amount must have up to six decimal places.");
    }

    const uint64_t whole = std::stoull(whole_text);
    std::string padded_fraction = fraction_text;
    padded_fraction.append(6 - padded_fraction.size(), '0');
    const uint64_t fraction = padded_fraction.empty() ? 0 : std::stoull(padded_fraction);
    if (whole > (std::numeric_limits<uint64_t>::max() - fraction) / 1000000) {
        throw std::out_of_range("Amount is too large.");
    }
    const uint64_t amount = whole * 1000000 + fraction;
    if (amount == 0) throw std::invalid_argument("Amount must be positive.");
    return amount;
}

void handle_send_command(Node& node, const std::string& wallet_path, uint64_t& next_nonce) {
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::string to_address;
    std::string amount_str;

    std::cout << "Type address for send NTR:" << std::endl;
    std::getline(std::cin, to_address);
    to_address.erase(std::remove(to_address.begin(), to_address.end(), ' '), to_address.end());

    try {
        if (to_address.empty()) {
            throw std::runtime_error("Error: recipient address is empty.");
        }

        std::cout << "Amount of NTR to send to " << to_address << ": ";
        std::getline(std::cin, amount_str);
        const uint64_t amount = parse_ntr_amount(amount_str);

        std::string private_key;
        std::string public_key;
        std::string sender_address;
        if (!wallet_opening(wallet_path, &private_key, &public_key, &sender_address)) {
            throw std::runtime_error("Error: cannot open selected wallet.");
        }

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Transaction transaction(sender_address, to_address, amount, next_nonce, now);
        if (!transaction.generate_signature(private_key)) {
            throw std::runtime_error("Error: transaction signing failed; check the wallet key and address.");
        }

        const SubmissionResult result = node.submit_transaction(transaction);
        if (result == SubmissionResult::RejectedInvalid) {
            std::cout << "Transaction rejected: signature or transaction data is invalid." << std::endl;
        } else if (result == SubmissionResult::AlreadyKnown) {
            std::cout << "Transaction is already known by this node." << std::endl;
        } else {
            ++next_nonce;
            std::cout << "Transaction " << transaction.getTxID() << " accepted into local mempool." << std::endl;
            if (result == SubmissionResult::AcceptedAndBroadcast) {
                std::cout << "Transaction sent to a peer; waiting for block confirmation." << std::endl;
            } else {
                std::cout << "Transaction remains local until a peer is available." << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cout << error.what() << std::endl;
    }
}

void handle_mining_command() {
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cout << "Mining process initialization..." << std::endl;
}

int main() {
    std::filesystem::path dir = "wallets";
    Node node;
    Blockchain blockchain;
    uint64_t next_nonce = 0;

    if (!std::filesystem::exists(dir)) {
        std::cout << "Dir Error: " << dir << " isn't created." << std::endl;
        std::filesystem::create_directory(dir);
        std::cout << "Dir: " << dir << " successfully created." << std::endl;
    }

    std::cout << "NTR_0.1.0 node skeleton" << std::endl;
    std::cout << "Genesis block: " << blockchain.get_latest_block().get_hash() << std::endl;

    if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
        std::vector<std::string> wallets;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            wallets.push_back(entry.path().filename().string());
        }

        std::sort(wallets.begin(), wallets.end(), [](const std::string& first, const std::string& second) {
            auto number = [](const std::string& value) {
                return std::stoul(value.substr(value.find_first_of("0123456789")));
            };
            return number(first) < number(second);
        });

        std::cout << "0. Create new wallet" << std::endl;
        for (std::size_t i = 0; i < wallets.size(); ++i) {
            std::cout << i + 1 << ". " << wallets[i] << std::endl;
        }

        std::cout << "Select your wallets (0-" << wallets.size() << "): " << std::endl;
        int choice = 0;
        if (std::cin >> choice && choice >= 0 && choice <= static_cast<int>(wallets.size())) {
            if (choice == 0) {
                Wallet new_wallet;
                std::cout << "New wallet is successfully created." << std::endl;
                return 0;
            }

            const std::string selected_wallet = (dir / wallets[choice - 1]).string();
            std::cout << "Selected wallet: " << selected_wallet << "\n" << std::endl;
            while (true) {
                std::string move;
                std::cout << "Select move: 'send', 'peer', 'listen', 'mining', 'get', 'exit'" << std::endl;
                if (!(std::cin >> move)) {
                    std::cout << "Input error" << std::endl;
                    return 1;
                }
                if (move == "exit") {
                    std::cout << "Closing wallet..." << std::endl;
                    break;
                }
                if (move == "send") {
                    handle_send_command(node, selected_wallet, next_nonce);
                } else if (move == "peer") {
                    std::string host;
                    unsigned int peer_port = 0;
                    std::cout << "Peer host and port: ";
                    if (std::cin >> host >> peer_port && peer_port > 0 && peer_port <= 65535) {
                        if (node.add_peer(host, static_cast<uint16_t>(peer_port))) {
                            std::cout << "Peer added: " << host << ':' << peer_port << std::endl;
                        } else {
                            std::cout << "Peer was not added (invalid or already configured)." << std::endl;
                        }
                    } else {
                        std::cout << "Error: expected a host and port from 1 to 65535." << std::endl;
                        std::cin.clear();
                        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    }
                } else if (move == "listen") {
                    unsigned int listen_port = 0;
                    std::cout << "Listening port: ";
                    if (std::cin >> listen_port && listen_port > 0 && listen_port <= 65535) {
                        std::cout << (node.start(static_cast<uint16_t>(listen_port))
                            ? "Node is listening" : "Could not start listener") << std::endl;
                    } else {
                        std::cout << "Error: expected a port from 1 to 65535." << std::endl;
                        std::cin.clear();
                        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    }
                } else if (move == "mining") {
                    handle_mining_command();
                } else if (move == "get") {
                    std::string address;
                    if (wallet_opening(selected_wallet, nullptr, nullptr, &address)) {
                        std::cout << "Your address: " << address << std::endl;
                    } else {
                        std::cout << "Cannot open wallet file" << std::endl;
                    }
                } else {
                    std::cout << "Error: incorrect move. Try again." << std::endl;
                }
                std::cout << std::endl;
            }
        } else {
            std::cout << "Error: invalid wallet choice." << std::endl;
        }
    }
    return 0;
}