#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <array>
#include <sstream>
#include <chrono>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/ripemd.h>

class BlockOfBlockChain {
    private:
        std::vector<std::string> transaction;
        std::string merkle_root;
        std::string block_hash;
        std::string prev_block_hash; 
        int64_t timestamp;
        int64_t index;
        uint64_t nonce;
        
        
    public:
        std::vector<std::string> generate_genesis_block() {
            prev_block_hash == "0000000000000000000000000000000000000000000000000000000000000000";
            merkle_root == юж бюд"";

        }
        
        void add_transaction(const std::string&tx) {
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            
        }

        BlockOfBlockChain(){
        };
    
};