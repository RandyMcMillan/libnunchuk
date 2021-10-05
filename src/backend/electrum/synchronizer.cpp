// Copyright (c) 2020 Enigmo
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <backend/electrum/synchronizer.h>
#include <utils/addressutils.hpp>

using namespace boost::asio;
using json = nlohmann::json;

namespace nunchuk {

static int RECONNECT_DELAY_SECOND = 3;
static long long SUBCRIBE_DELAY_MS = 100;

ElectrumSynchronizer::~ElectrumSynchronizer() {
  std::lock_guard<std::mutex> guard(status_mutex_);
  status_ = Status::STOPPED;
  status_cv_.notify_all();
  sync_worker_.reset();
  sync_thread_.join();
}

void ElectrumSynchronizer::WaitForReady() {
  std::unique_lock<std::mutex> lock_(status_mutex_);
  status_cv_.wait(lock_, [&]() {
    return status_ == Status::READY || status_ == Status::SYNCING;
  });
}

void ElectrumSynchronizer::Run() {
  {
    std::lock_guard<std::mutex> guard(status_mutex_);
    if (status_ == Status::STOPPED) return;
    status_ = Status::CONNECTING;
    status_cv_.notify_all();
  }
  // Clear cache
  chain_tip_ = 0;
  scripthash_to_wallet_address_.clear();

  io_service_.post([&]() {
    try {
      client_ = std::unique_ptr<ElectrumClient>(
          new ElectrumClient(app_settings_, [&]() {
            io_service_.post([&]() {
              std::this_thread::sleep_for(
                  std::chrono::seconds(RECONNECT_DELAY_SECOND));
              Run();
            });
          }));
    } catch (...) {
      std::lock_guard<std::mutex> guard(status_mutex_);
      status_ = Status::UNINITIALIZED;
      status_cv_.notify_all();
      return;
    }
    {
      std::lock_guard<std::mutex> guard(status_mutex_);
      if (status_ != Status::CONNECTING) return;
      status_ = Status::SYNCING;
      status_cv_.notify_all();
    }
    try {
      BlockchainSync(app_settings_.get_chain());
    } catch (...) {
      // TODO(Bakaoh): more elegant exeption handling
      // storage and CoreUtils chain-switch may cause exeption here
    }
    std::lock_guard<std::mutex> guard(status_mutex_);
    if (status_ != Status::SYNCING) return;
    status_ = Status::READY;
    status_cv_.notify_all();
  });
}

void ElectrumSynchronizer::UpdateTransactions(Chain chain,
                                              const std::string& wallet_id,
                                              const json& history) {
  if (!history.is_array()) return;
  for (auto it = history.begin(); it != history.end(); ++it) {
    json item = it.value();
    std::string tx_id = item["tx_hash"];
    int height = item["height"];
    try {
      // TODO(Bakaoh): [optimize] use GetTransactions
      Transaction tx = storage_->GetTransaction(chain, wallet_id, tx_id);
      if (tx.get_status() != TransactionStatus::CONFIRMED && height > 0) {
        auto tx = client_->blockchain_transaction_get(tx_id);
        storage_->UpdateTransaction(chain, wallet_id, tx["hex"], height,
                                    tx["blocktime"]);
        transaction_listener_(tx_id, TransactionStatus::CONFIRMED, wallet_id);
      }
    } catch (StorageException& se) {
      if (se.code() == StorageException::TX_NOT_FOUND) {
        auto tx = client_->blockchain_transaction_get(tx_id);
        time_t time = tx["blocktime"] == nullptr ? 0 : time_t(tx["blocktime"]);
        Amount fee = 0;
        if (height <= 0) {
          height = 0;
          fee = Amount(item["fee"]);
        }
        storage_->InsertTransaction(chain, wallet_id, tx["hex"], height, time,
                                    fee);
        auto status = height <= 0 ? TransactionStatus::PENDING_CONFIRMATION
                                  : TransactionStatus::CONFIRMED;
        transaction_listener_(tx_id, status, wallet_id);
      }
    }
  }
}

void ElectrumSynchronizer::OnScripthashStatusChange(Chain chain,
                                                    const json& notification) {
  std::string scripthash = notification[0];
  if (scripthash_to_wallet_address_.count(scripthash) == 0) return;
  std::string wallet_id = scripthash_to_wallet_address_.at(scripthash).first;
  std::string address = scripthash_to_wallet_address_.at(scripthash).second;
  json utxo = client_->blockchain_scripthash_listunspent(scripthash);
  storage_->SetUtxos(chain, wallet_id, address, utxo.dump());
  json history = client_->blockchain_scripthash_get_history(scripthash);
  UpdateTransactions(chain, wallet_id, history);
  Amount balance = storage_->GetBalance(chain, wallet_id);
  balance_listener_(wallet_id, balance);
}

std::string ElectrumSynchronizer::SubscribeAddress(const std::string& wallet_id,
                                                   const std::string& address) {
  std::string scripthash = AddressToScriptHash(address);
  scripthash_to_wallet_address_[scripthash] = {wallet_id, address};
  client_->blockchain_scripthash_subscribe(scripthash);
  return scripthash;
}

void ElectrumSynchronizer::BlockchainSync(Chain chain) {
  connection_listener_(ConnectionStatus::OFFLINE, 0);
  {
    std::unique_lock<std::mutex> lock_(status_mutex_);
    if (status_ != Status::READY && status_ != Status::SYNCING) return;
    auto header = client_->blockchain_headers_subscribe([&](json rs) {
      chain_tip_ = rs[0]["height"];
      storage_->SetChainTip(app_settings_.get_chain(), chain_tip_);
      block_listener_(rs[0]["height"], rs[0]["hex"]);
    });
    connection_listener_(ConnectionStatus::SYNCING, 0);
    chain_tip_ = header["height"];
    storage_->SetChainTip(chain, header["height"]);
    block_listener_(header["height"], header["hex"]);
    client_->scripthash_add_listener([&](json notification) {
      OnScripthashStatusChange(app_settings_.get_chain(), notification);
    });
  }
  auto wallet_ids = storage_->ListWallets(chain);
  int process = 0;
  for (auto i = wallet_ids.rbegin(); i != wallet_ids.rend(); ++i) {
    auto wallet_id = *i;
    auto addresses = storage_->GetAllAddresses(chain, wallet_id);
    for (auto a = addresses.rbegin(); a != addresses.rend(); ++a) {
      std::unique_lock<std::mutex> lock_(status_mutex_);
      if (status_ != Status::READY && status_ != Status::SYNCING) return;
      auto address = *a;
      auto scripthash = SubscribeAddress(wallet_id, address);
      json utxo = client_->blockchain_scripthash_listunspent(scripthash);
      storage_->SetUtxos(chain, wallet_id, address, utxo.dump());
      json history = client_->blockchain_scripthash_get_history(scripthash);
      UpdateTransactions(chain, wallet_id, history);
      std::this_thread::sleep_for(std::chrono::milliseconds(SUBCRIBE_DELAY_MS));
    }
    Amount balance = storage_->GetBalance(chain, wallet_id);
    balance_listener_(wallet_id, balance);
    connection_listener_(ConnectionStatus::SYNCING,
                         ++process * 100 / wallet_ids.size());
  }
  connection_listener_(ConnectionStatus::ONLINE, 100);
}

void ElectrumSynchronizer::Broadcast(const std::string& raw_tx) {
  std::unique_lock<std::mutex> lock_(status_mutex_);
  if (status_ != Status::READY && status_ != Status::SYNCING) {
    throw NunchukException(NunchukException::SERVER_REQUEST_ERROR,
                           "Disconnected");
  }
  client_->blockchain_transaction_broadcast(raw_tx);
}

Amount ElectrumSynchronizer::EstimateFee(int conf_target) {
  std::unique_lock<std::mutex> lock_(status_mutex_);
  if (status_ != Status::READY && status_ != Status::SYNCING) {
    throw NunchukException(NunchukException::SERVER_REQUEST_ERROR,
                           "Disconnected");
  }
  return Utils::AmountFromValue(
      client_->blockchain_estimatefee(conf_target).dump());
}

Amount ElectrumSynchronizer::RelayFee() {
  std::unique_lock<std::mutex> lock_(status_mutex_);
  if (status_ != Status::READY && status_ != Status::SYNCING) {
    throw NunchukException(NunchukException::SERVER_REQUEST_ERROR,
                           "Disconnected");
  }
  return Utils::AmountFromValue(client_->blockchain_relayfee().dump());
}

bool ElectrumSynchronizer::LookAhead(Chain chain, const std::string& wallet_id,
                                     const std::string& address, int index,
                                     bool internal) {
  std::unique_lock<std::mutex> lock_(status_mutex_);
  if (status_ != Status::READY && status_ != Status::SYNCING) return false;
  if (chain != app_settings_.get_chain()) return false;

  auto scripthash = SubscribeAddress(wallet_id, address);
  json history = client_->blockchain_scripthash_get_history(scripthash);
  if (!history.is_array() || history.empty()) return false;
  storage_->AddAddress(chain, wallet_id, address, index, internal);
  UpdateTransactions(chain, wallet_id, history);
  json utxo = client_->blockchain_scripthash_listunspent(scripthash);
  storage_->SetUtxos(chain, wallet_id, address, utxo.dump());
  return true;
}

void ElectrumSynchronizer::RescanBlockchain(int start_height, int stop_height) {
}

}  // namespace nunchuk