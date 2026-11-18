#include "server.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    uint16_t port = 4173;
    if (argc > 1) {
        try {
            const int parsed_port = std::stoi(argv[1]);
            if (parsed_port < 1 || parsed_port > 65535) {
                throw std::runtime_error("Port must be from 1 to 65535");
            }
            port = static_cast<uint16_t>(parsed_port);
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << std::endl;
            return 1;
        }
    }

    DashboardServer server(port);
    if (!server.initialize()) {
        std::cerr << "Error: cannot initialize dashboard database" << std::endl;
        return 1;
    }

    std::cout << "NTR dashboard is available at http://127.0.0.1:" << port << std::endl;
    if (!server.run()) {
        std::cerr << "Error: cannot start dashboard server" << std::endl;
        return 1;
    }
    return 0;
}
