CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Wno-deprecated-declarations -pthread
LDLIBS ?= -lsqlite3 -lcrypto -pthread

SOURCES = main.cpp server.cpp \
	../blockchain/block.cpp \
	../blockchain/blockchain.cpp \
	../blockchain/mempool.cpp \
	../blockchain/transactions/transaction.cpp \
	../crypto/helper.cpp \
	../network/node.cpp \
	../wallet/wallet.cpp

CONSOLE_SOURCES = ../console_main.cpp \
	../blockchain/block.cpp \
	../blockchain/blockchain.cpp \
	../blockchain/mempool.cpp \
	../blockchain/transactions/transaction.cpp \
	../crypto/helper.cpp \
	../network/node.cpp \
	../wallet/wallet.cpp

dashboard_server: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDLIBS) -o dashboard_server

console: $(CONSOLE_SOURCES)
	$(CXX) $(CXXFLAGS) $(CONSOLE_SOURCES) $(LDLIBS) -o ntr_console

clean:
	rm -f dashboard_server ntr_console

.PHONY: clean console