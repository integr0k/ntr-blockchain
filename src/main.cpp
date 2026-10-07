#include "wallet/wallet.hpp"
#include "crypto/helper.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>
#include <stdexcept>

void handle_send_command() {
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    std::string to_address;
    std::string amount_str;

    std::cout << "Type address for send NTR:" << std::endl;
    std::getline(std::cin, to_address);
    to_address.erase(std::remove(to_address.begin(), to_address.end(), ' '), to_address.end());

    // Весь твой try-catch теперь живет локально здесь
    try {
        if (to_address.size() != 42) {
            throw std::runtime_error("Error: NTR address is incorrect. " + std::to_string(to_address.size()) + " bytes.");
        }
        
        std::cout << "Amount of NTR to send to " << to_address << ": ";
        std::getline(std::cin, amount_str);

        try {
            int amount_to_send = std::stoi(amount_str);
            if (amount_to_send > 1) { // Твоя проверка баланса
                throw std::runtime_error("Oops! You don't have the required amount of NTR.");
            }
            
            // Если ошибок нет — выполняем отправку
            std::cout << "Successfully sent " << amount_to_send << " NTR!" << std::endl;

        } catch (const std::invalid_argument&) {
            std::cout << "Error: Invalid number format!" << std::endl;
        }
    } 
    catch(const std::exception& e) {
        std::cout << e.what() << std::endl; 
    }
    // Функция завершилась, мы автоматически возвращаемся в меню!
}

int main() {
   std::filesystem::path dir = "wallets";

    if (!std::filesystem::exists(dir)) {
        std::cout << "Dir Error: " << dir << " isn't created." << std::endl;
        std::filesystem::create_directory(dir);
        std::cout << "Dir: " << dir << " successfully created." << std::endl;
    }

    std::cout << "NTR_0.1.0 node skeleton" << std::endl;

    if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
        std::vector<std::string> wallets;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            wallets.push_back(entry.path().filename().string());
        }

        std::sort(wallets.begin(), wallets.end(), [](const std::string& a, const std::string& b) {
            auto num = [](const std::string& s) {
                return std::stoi(s.substr(s.find_first_of("0123456789")));
            };
            return num(a) < num(b);
        });

        std::cout << "0. Create new wallet" << std::endl;
        for (std::size_t i = 0; i < wallets.size(); ++i) {
            std::cout << i + 1 << ". " << wallets[i] << std::endl;
        }

        std::cout << "Select your wallets (0-" << wallets.size() << "): " << std::endl;
        int choice = 0;

        if (std::cin >> choice && choice >= 0 && choice <= static_cast<int>(wallets.size())) {
            if (choice == 0) {
                std::cout << "Create new wallet" << std::endl;
                Wallet new_wallet;
                std::cout << "New wallet is successfully created." << std::endl;
                return 0;
            }

            std::string selected_wallet = (dir / wallets[choice - 1]).string();
            std::cout << "Selected wallet: " << selected_wallet << "\n" << std::endl;

            // 2. В ЦИКЛ МЫ ОБОРАЧИВАЕМ ТОЛЬКО ВЫБОР ДЕЙСТВИЙ (МЕНЮ)
            while (true) {
                std::string move;
                std::cout << "Select move: 'send', 'mining', 'get', 'exit'" << std::endl;

                if (!(std::cin >> move)) {
                    std::cout << "Input error" << std::endl;
                    return 1;
                }

                if (move == "exit") {
                    std::cout << "Closing wallet..." << std::endl;
                    break; // Выход из цикла и завершение программы
                }

                if (move == "send") {
                    handle_send_command(); // Просто вызываем функцию! Она отработает, поймает ошибки и вернет нас сюда.
                }
                else if (move == "mining") {
                    std::cout << "Mining process initialization..." << std::endl;
                }
                else if (move == "get") {
                    std::string private_key, public_key, address;
                    if (wallet_opening(selected_wallet, &private_key, &public_key, &address)) {
                        std::cout << "Your address: " << address << std::endl;
                    } else {
                        std::cout << "Cannot open wallet file" << std::endl;
                    }
                }
                else {
                    std::cout << "Error: incorrect move. Try again." << std::endl;
                }
                std::cout << std::endl; // Пустая строка для красоты между командами
            }
        } else {
            std::cout << "Error: invalid wallet choice." << std::endl;
        }
    }

    return 0;
}