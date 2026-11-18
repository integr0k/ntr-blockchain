# NTR local dashboard

Build and run from the repository's `src` directory:

```sh
make -C dashboard
./dashboard/dashboard_server
```

Build the console wallet manager separately if needed:

```sh
make -C dashboard console
./dashboard/ntr_console
```

Open <http://127.0.0.1:4173>. The server binds to loopback. Pass another port as an argument when needed:

```sh
./dashboard/dashboard_server 4174
```

The SQLite database is created at `dashboard/data/network.sqlite` and survives restarts:

- `blocks` stores the complete local block ledger, including header version, hashes, proof-of-work nonce, Merkle root inputs, payout address, and reward.
- `transactions` stores coinbase payouts and confirmed signed transfers.
- `pending_transactions` persists signed transactions waiting for a block.
- `balances` is the precomputed balance index; confirmed sends debit the sender and credit the recipient atomically with the block.
- `peers` stores requested endpoints and their latest TCP reachability status.

The C++ server uses `Wallet`, `Transaction`, `Node`, `BlockOfBlockChain`, and `Blockchain`. The wallet manager lists and creates wallets, signs sends on the server, and submits them to the node mempool. Private keys are never sent to the browser. Amounts support six decimal places; mining uses SHA-256 proof of work and credits 50 NTR plus included fees. New v4 blocks retarget every six v4 blocks toward 12 seconds per block, changing one leading zero per window within a range of three to seven. The page polls `/api/state`; wallet endpoints are `/api/wallets` and `/api/transactions`.

The previous console entry point is now `console_main.cpp`; the browser UI is the default local manager. The database and generated server binary are local runtime files. Header v1/v2/v3 remain readable for existing records; newly mined v4 blocks commit transfer Merkle roots and total fees. Peer status means TCP reachability, not a completed chain handshake. Block synchronization, fork choice, full recipient-address checksum validation, and production-grade peer security are still needed for a real network. Wallet files still store private keys as plaintext on disk.
