// Copyright (c) 2020 Enigmo
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "storage.h"

#include <descriptor.h>
#include <utils/bip32.hpp>
#include <utils/txutils.hpp>
#include <utils/json.hpp>
#include <utils/loguru.hpp>
#include <utils/bsms.hpp>
#include <boost/filesystem/string_file.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/thread/locks.hpp>
#include <set>
#include <sstream>
#include <cstring>
#include <algorithm>

#include <univalue.h>
#include <rpc/util.h>
#include <policy/policy.h>
#include <crypto/sha256.h>

#ifdef _WIN32
#include <shlobj.h>
#endif

using json = nlohmann::json;
namespace fs = boost::filesystem;
namespace ba = boost::algorithm;

namespace nunchuk {

fs::path NunchukStorage::GetDefaultDataDir() const {
  // Windows: C:\Users\Username\AppData\Roaming\Nunchuk
  // Mac: ~/Library/Application Support/Nunchuk
  // Unix: ~/.nunchuk
#ifdef _WIN32
  // Windows
  WCHAR pszPath[MAX_PATH] = L"";
  if (SHGetSpecialFolderPathW(nullptr, pszPath, CSIDL_APPDATA, true)) {
    return fs::path(pszPath) / "Nunchuk";
  }
  return fs::path("Nunchuk");
#else
  fs::path pathRet;
  char* pszHome = getenv("HOME");
  if (pszHome == nullptr || std::strlen(pszHome) == 0)
    pathRet = fs::path("/");
  else
    pathRet = fs::path(pszHome);
#ifdef __APPLE__
  // Mac
  return pathRet / "Library/Application Support/Nunchuk";
#else
  // Unix
  return pathRet / ".nunchuk";
#endif
#endif
}

bool NunchukStorage::WriteFile(const std::string& file_path,
                               const std::string& value) {
  fs::save_string_file(fs::system_complete(file_path), value);
  return true;
}

std::string NunchukStorage::LoadFile(const std::string& file_path) {
  std::string value;
  fs::load_string_file(fs::system_complete(file_path), value);
  return value;
}

bool NunchukStorage::ExportWallet(Chain chain, const std::string& wallet_id,
                                  const std::string& file_path,
                                  ExportFormat format) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  auto wallet_db = GetWalletDb(chain, wallet_id);
  switch (format) {
    case ExportFormat::COLDCARD:
      return WriteFile(file_path, wallet_db.GetMultisigConfig());
    case ExportFormat::DESCRIPTOR: {
      return WriteFile(
          file_path, wallet_db.GetWallet().get_descriptor(DescriptorPath::ANY));
    }
    case ExportFormat::BSMS: {
      return WriteFile(file_path, GetDescriptorRecord(wallet_db.GetWallet()));
    }
    case ExportFormat::DB:
      if (passphrase_.empty()) {
        fs::copy_file(GetWalletDir(chain, wallet_id), file_path);
      } else {
        wallet_db.DecryptDb(file_path);
      }
      return true;
    default:
      return false;
  }
}

std::string NunchukStorage::ImportWalletDb(Chain chain,
                                           const std::string& file_path) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  auto wallet_db = NunchukWalletDb{chain, "", file_path, ""};
  std::string id = wallet_db.GetId();
  auto wallet_file = GetWalletDir(chain, id);
  if (fs::exists(wallet_file)) {
    throw StorageException(StorageException::WALLET_EXISTED, "wallet existed!");
  }
  wallet_db.EncryptDb(wallet_file.string(), passphrase_);
  return id;
}

NunchukStorage::NunchukStorage(const std::string& datadir,
                               const std::string& passphrase,
                               const std::string& account)
    : passphrase_(passphrase), account_(account) {
  if (!datadir.empty()) {
    datadir_ = fs::system_complete(datadir);
    if (!fs::is_directory(datadir_)) {
      throw StorageException(StorageException::INVALID_DATADIR,
                             "datadir is not directory!");
    }
  } else {
    datadir_ = GetDefaultDataDir();
  }

  if (!account_.empty()) {
    CSHA256 hasher;
    std::vector<unsigned char> stream(account_.begin(), account_.end());
    hasher.Write((unsigned char*)&(*stream.begin()),
                 stream.end() - stream.begin());
    uint256 hash;
    hasher.Finalize(hash.begin());
    datadir_ = datadir_ / hash.GetHex();
  }
  if (fs::create_directories(datadir_ / "testnet")) {
    fs::create_directories(datadir_ / "testnet" / "wallets");
    fs::create_directories(datadir_ / "testnet" / "signers");
  }
  if (fs::create_directories(datadir_ / "mainnet")) {
    fs::create_directories(datadir_ / "mainnet" / "wallets");
    fs::create_directories(datadir_ / "mainnet" / "signers");
  }
  fs::create_directories(datadir_ / "tmp");
}

void NunchukStorage::SetPassphrase(const std::string& value) {
  if (value == passphrase_) {
    throw NunchukException(NunchukException::PASSPHRASE_ALREADY_USED,
                           "passphrase used");
  }
  SetPassphrase(Chain::MAIN, value);
  SetPassphrase(Chain::TESTNET, value);
  passphrase_ = value;
}

void NunchukStorage::SetPassphrase(Chain chain, const std::string& value) {
  auto wallets = ListWallets(chain);
  auto signers = ListMasterSigners(chain);
  boost::unique_lock<boost::shared_mutex> lock(access_);
  if (passphrase_.empty()) {
    for (auto&& wallet_id : wallets) {
      auto old_file = GetWalletDir(chain, wallet_id);
      auto new_file = datadir_ / "tmp" / wallet_id;
      GetWalletDb(chain, wallet_id).EncryptDb(new_file.string(), value);
      fs::copy_file(new_file, old_file, fs::copy_option::overwrite_if_exists);
      fs::remove(new_file);
    }
    for (auto&& signer_id : signers) {
      auto old_file = GetSignerDir(chain, signer_id);
      auto new_file = datadir_ / "tmp" / signer_id;
      GetSignerDb(chain, signer_id).EncryptDb(new_file.string(), value);
      fs::copy_file(new_file, old_file, fs::copy_option::overwrite_if_exists);
      fs::remove(new_file);
    }
    {
      auto old_file = GetRoomDir(chain);
      auto new_file = datadir_ / "tmp" / "matrix";
      GetRoomDb(chain).EncryptDb(new_file.string(), value);
      fs::copy_file(new_file, old_file, fs::copy_option::overwrite_if_exists);
      fs::remove(new_file);
    }
  } else if (value.empty()) {
    for (auto&& wallet_id : wallets) {
      auto old_file = GetWalletDir(chain, wallet_id);
      auto new_file = datadir_ / "tmp" / wallet_id;
      GetWalletDb(chain, wallet_id).DecryptDb(new_file.string());
      fs::copy_file(new_file, old_file, fs::copy_option::overwrite_if_exists);
      fs::remove(new_file);
    }
    for (auto&& signer_id : signers) {
      auto old_file = GetSignerDir(chain, signer_id);
      auto new_file = datadir_ / "tmp" / signer_id;
      GetSignerDb(chain, signer_id).DecryptDb(new_file.string());
      fs::copy_file(new_file, old_file, fs::copy_option::overwrite_if_exists);
      fs::remove(new_file);
    }
    {
      auto old_file = GetRoomDir(chain);
      auto new_file = datadir_ / "tmp" / "matrix";
      GetRoomDb(chain).DecryptDb(new_file.string());
      fs::copy_file(new_file, old_file, fs::copy_option::overwrite_if_exists);
      fs::remove(new_file);
    }
  } else {
    for (auto&& wallet_id : wallets) {
      GetWalletDb(chain, wallet_id).ReKey(value);
    }
    for (auto&& signer_id : signers) {
      GetSignerDb(chain, signer_id).ReKey(value);
    }
    GetRoomDb(chain).ReKey(value);
  }
}

std::string NunchukStorage::ChainStr(Chain chain) const {
  if (chain == Chain::TESTNET) {
    return "testnet";
  } else if (chain == Chain::REGTEST) {
    return "regtest";
  }
  return "mainnet";
}

fs::path NunchukStorage::GetWalletDir(Chain chain,
                                      const std::string& id) const {
  return datadir_ / ChainStr(chain) / "wallets" / id;
}

fs::path NunchukStorage::GetSignerDir(Chain chain,
                                      const std::string& id) const {
  return datadir_ / ChainStr(chain) / "signers" / id;
}

fs::path NunchukStorage::GetAppStateDir(Chain chain) const {
  return datadir_ / ChainStr(chain) / "state";
}

fs::path NunchukStorage::GetRoomDir(Chain chain) const {
  return datadir_ / ChainStr(chain) / "room";
}

NunchukWalletDb NunchukStorage::GetWalletDb(Chain chain,
                                            const std::string& id) {
  fs::path db_file = GetWalletDir(chain, id);
  if (!fs::exists(db_file)) {
    throw StorageException(StorageException::WALLET_NOT_FOUND,
                           "wallet not exists!");
  }
  return NunchukWalletDb{chain, id, db_file.string(), passphrase_};
}

NunchukSignerDb NunchukStorage::GetSignerDb(Chain chain,
                                            const std::string& id) {
  fs::path db_file = GetSignerDir(chain, id);
  if (!fs::exists(db_file)) {
    throw StorageException(StorageException::MASTERSIGNER_NOT_FOUND,
                           "signer not exists!");
  }
  return NunchukSignerDb{chain, id, db_file.string(), passphrase_};
}

NunchukAppStateDb NunchukStorage::GetAppStateDb(Chain chain) {
  fs::path db_file = GetAppStateDir(chain);
  bool is_new = !fs::exists(db_file);
  auto db = NunchukAppStateDb{chain, "", db_file.string(), ""};
  if (is_new) db.Init();
  return db;
}

NunchukRoomDb NunchukStorage::GetRoomDb(Chain chain) {
  fs::path db_file = GetRoomDir(chain);
  bool is_new = !fs::exists(db_file);
  auto db = NunchukRoomDb{chain, "", db_file.string(), passphrase_};
  if (is_new) db.Init();
  return db;
}

Wallet NunchukStorage::CreateWallet(Chain chain, const std::string& name, int m,
                                    int n,
                                    const std::vector<SingleSigner>& signers,
                                    AddressType address_type, bool is_escrow,
                                    const std::string& description,
                                    bool allow_used_signer) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return CreateWallet0(chain, name, m, n, signers, address_type, is_escrow,
                       description, allow_used_signer, std::time(0));
}

Wallet NunchukStorage::CreateWallet0(Chain chain, const std::string& name,
                                     int m, int n,
                                     const std::vector<SingleSigner>& signers,
                                     AddressType address_type, bool is_escrow,
                                     const std::string& description,
                                     bool allow_used_signer,
                                     time_t create_date) {
  if (m > n) {
    throw NunchukException(NunchukException::INVALID_PARAMETER,
                           "invalid parameter: m > n");
  }
  if (n != signers.size()) {
    throw NunchukException(NunchukException::INVALID_PARAMETER,
                           "invalid parameter: n and signers are not match");
  }
  WalletType wallet_type =
      n == 1 ? WalletType::SINGLE_SIG
             : (is_escrow ? WalletType::ESCROW : WalletType::MULTI_SIG);
  for (auto&& signer : signers) {
    auto master_id = signer.get_master_fingerprint();
    NunchukSignerDb signer_db{
        chain, master_id, GetSignerDir(chain, master_id).string(), passphrase_};
    if (signer_db.IsMaster() && !signer.get_xpub().empty()) {
      int index = GetIndexFromPath(signer.get_derivation_path());
      if (FormalizePath(
              GetBip32Path(chain, wallet_type, address_type, index)) !=
          FormalizePath(signer.get_derivation_path())) {
        throw NunchukException(NunchukException::INVALID_BIP32_PATH,
                               "invalid bip32 path!");
      }
      signer_db.AddXPub(wallet_type, address_type, index, signer.get_xpub());
      if (!signer_db.UseIndex(wallet_type, address_type, index) &&
          !allow_used_signer) {
        throw StorageException(StorageException::SIGNER_USED, "signer used!");
      }
    } else {
      try {
        signer_db.GetRemoteSigner(signer.get_derivation_path());
        signer_db.UseRemote(signer.get_derivation_path());
      } catch (StorageException& se) {
        if (se.code() == StorageException::SIGNER_NOT_FOUND) {
          signer_db.AddRemote("import", signer.get_xpub(),
                              signer.get_public_key(),
                              signer.get_derivation_path(), true);
        } else {
          throw;
        }
      }
    }
  }
  std::string external_desc = GetDescriptorForSigners(
      signers, m, DescriptorPath::EXTERNAL_ALL, address_type, wallet_type);
  std::string id = GetDescriptorChecksum(external_desc);
  fs::path wallet_file = GetWalletDir(chain, id);
  if (fs::exists(wallet_file)) {
    throw StorageException(StorageException::WALLET_EXISTED, "wallet existed!");
  }
  NunchukWalletDb wallet_db{chain, id, wallet_file.string(), passphrase_};
  wallet_db.InitWallet(name, m, n, signers, address_type, is_escrow,
                       create_date, description);
  Wallet wallet(id, m, n, signers, address_type, is_escrow, create_date);
  wallet.set_name(name);
  wallet.set_description(description);
  wallet.set_balance(0);
  return wallet;
}

std::string NunchukStorage::CreateMasterSigner(Chain chain,
                                               const std::string& name,
                                               const Device& device,
                                               const std::string& mnemonic) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  std::string id = ba::to_lower_copy(device.get_master_fingerprint());
  NunchukSignerDb signer_db{chain, id, GetSignerDir(chain, id).string(),
                            passphrase_};
  signer_db.InitSigner(name, device, mnemonic);
  return id;
}

SingleSigner NunchukStorage::CreateSingleSigner(
    Chain chain, const std::string& name, const std::string& xpub,
    const std::string& public_key, const std::string& derivation_path,
    const std::string& master_fingerprint) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  std::string id = master_fingerprint;
  NunchukSignerDb signer_db{chain, id, GetSignerDir(chain, id).string(),
                            passphrase_};
  if (signer_db.IsMaster()) {
    throw StorageException(StorageException::SIGNER_EXISTS, "signer exists");
  }
  if (!signer_db.AddRemote(name, xpub, public_key, derivation_path)) {
    throw StorageException(StorageException::SIGNER_EXISTS, "signer exists");
  }
  auto signer = SingleSigner(name, xpub, public_key, derivation_path,
                             master_fingerprint, 0);
  signer.set_type(SignerType::AIRGAP);
  return signer;
}

SingleSigner NunchukStorage::GetSignerFromMasterSigner(
    Chain chain, const std::string& mastersigner_id,
    const WalletType& wallet_type, const AddressType& address_type, int index) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  auto signer_db = GetSignerDb(chain, mastersigner_id);
  std::string path = GetBip32Path(chain, wallet_type, address_type, index);
  auto signer = SingleSigner(
      signer_db.GetName(), signer_db.GetXpub(wallet_type, address_type, index),
      "", path, signer_db.GetFingerprint(), signer_db.GetLastHealthCheck(),
      mastersigner_id);
  signer.set_type(signer_db.GetSignerType());
  return signer;
}

std::vector<SingleSigner> NunchukStorage::GetSignersFromMasterSigner(
    Chain chain, const std::string& mastersigner_id) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, mastersigner_id).GetSingleSigners();
}

void NunchukStorage::CacheMasterSignerXPub(
    Chain chain, const std::string& id,
    std::function<std::string(std::string)> getxpub,
    std::function<bool(int)> progress, bool first) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  auto signer_db = GetSignerDb(chain, id);

  int count = 0;
  auto total = first ? 7 : TOTAL_CACHE_NUMBER;
  progress(count++ * 100 / total);

  // Retrieve standard BIP32 paths when connected to a device for the first time
  if (first) {
    auto cachePath = [&](const std::string& path) {
      signer_db.AddXPub(path, getxpub(path), "custom");
      progress(count++ * 100 / total);
    };
    cachePath("m");
    cachePath(chain == Chain::MAIN ? MAINNET_HEALTH_CHECK_PATH
                                   : TESTNET_HEALTH_CHECK_PATH);
  }

  auto cacheIndex = [&](WalletType w, AddressType a, int n) {
    int index = signer_db.GetCachedIndex(w, a);
    if (index < 0 && w == WalletType::MULTI_SIG) index = 0;
    for (int i = index + 1; i <= index + n; i++) {
      signer_db.AddXPub(w, a, i, getxpub(GetBip32Path(chain, w, a, i)));
      progress(count++ * 100 / total);
    }
  };
  cacheIndex(WalletType::MULTI_SIG, AddressType::ANY,
             first ? 1 : MULTISIG_CACHE_NUMBER);
  cacheIndex(WalletType::SINGLE_SIG, AddressType::NATIVE_SEGWIT,
             first ? 1 : SINGLESIG_BIP84_CACHE_NUMBER);
  cacheIndex(WalletType::SINGLE_SIG, AddressType::NESTED_SEGWIT,
             first ? 1 : SINGLESIG_BIP49_CACHE_NUMBER);
  cacheIndex(WalletType::SINGLE_SIG, AddressType::LEGACY,
             first ? 1 : SINGLESIG_BIP48_CACHE_NUMBER);
  cacheIndex(WalletType::ESCROW, AddressType::ANY,
             first ? 1 : ESCROW_CACHE_NUMBER);
}

int NunchukStorage::GetCurrentIndexFromMasterSigner(
    Chain chain, const std::string& mastersigner_id,
    const WalletType& wallet_type, const AddressType& address_type) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, mastersigner_id)
      .GetUnusedIndex(wallet_type, address_type);
}

int NunchukStorage::GetCachedIndexFromMasterSigner(
    Chain chain, const std::string& mastersigner_id,
    const WalletType& wallet_type, const AddressType& address_type) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, mastersigner_id)
      .GetCachedIndex(wallet_type, address_type);
}

std::string NunchukStorage::GetMasterSignerXPub(
    Chain chain, const std::string& mastersigner_id, const std::string& path) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, mastersigner_id).GetXpub(path);
}

std::vector<std::string> NunchukStorage::ListWallets(Chain chain) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return ListWallets0(chain);
}

std::vector<std::string> NunchukStorage::ListWallets0(Chain chain) {
  fs::path directory = (datadir_ / ChainStr(chain) / "wallets");
  std::vector<std::string> ids;
  for (auto&& f : fs::directory_iterator(directory)) {
    auto id = f.path().filename().string();
    if (id.size() != 8) continue;
    ids.push_back(id);
  }
  return ids;
}

std::vector<std::string> NunchukStorage::ListMasterSigners(Chain chain) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return ListMasterSigners0(chain);
}

std::vector<std::string> NunchukStorage::ListMasterSigners0(Chain chain) {
  fs::path directory = (datadir_ / ChainStr(chain) / "signers");
  std::vector<std::string> ids;
  for (auto&& f : fs::directory_iterator(directory)) {
    auto id = f.path().filename().string();
    if (id.size() != 8) continue;
    ids.push_back(id);
  }
  return ids;
}

Wallet NunchukStorage::GetWallet(Chain chain, const std::string& id,
                                 bool create_signers_if_not_exist) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  auto wallet_db = GetWalletDb(chain, id);
  Wallet wallet = wallet_db.GetWallet();
  std::vector<SingleSigner> signers;

  for (auto&& signer : wallet.get_signers()) {
    std::string name = signer.get_name();
    std::string master_id = signer.get_master_fingerprint();
    time_t last_health_check = signer.get_last_health_check();
    NunchukSignerDb signer_db{
        chain, master_id, GetSignerDir(chain, master_id).string(), passphrase_};
    SignerType signer_type = signer_db.GetSignerType();
    if (signer_db.IsMaster()) {
      name = signer_db.GetName();
      last_health_check = signer_db.GetLastHealthCheck();
    } else {
      // master_id is used by the caller to check if the signer is master or
      // remote
      master_id = "";
      signer_type = SignerType::AIRGAP;
      try {
        auto remote = signer_db.GetRemoteSigner(signer.get_derivation_path());
        name = remote.get_name();
        last_health_check = remote.get_last_health_check();
      } catch (StorageException& se) {
        if (se.code() == StorageException::SIGNER_NOT_FOUND &&
            create_signers_if_not_exist) {
          signer_db.AddRemote(signer.get_name(), signer.get_xpub(),
                              signer.get_public_key(),
                              signer.get_derivation_path(), true);
        } else {
          throw;
        }
      }
    }
    SingleSigner true_signer(name, signer.get_xpub(), signer.get_public_key(),
                             signer.get_derivation_path(),
                             signer.get_master_fingerprint(), last_health_check,
                             master_id);
    true_signer.set_type(signer_type);
    signers.push_back(true_signer);
  }
  Wallet true_wallet(id, wallet.get_m(), wallet.get_n(), signers,
                     wallet.get_address_type(), wallet.is_escrow(),
                     wallet.get_create_date());
  true_wallet.set_name(wallet.get_name());
  true_wallet.set_balance(wallet.get_balance());
  return true_wallet;
}

MasterSigner NunchukStorage::GetMasterSigner(Chain chain,
                                             const std::string& id) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  auto mid = ba::to_lower_copy(id);
  auto signer_db = GetSignerDb(chain, mid);
  Device device{signer_db.GetDeviceType(), signer_db.GetDeviceModel(),
                signer_db.GetFingerprint()};
  SignerType signer_type = signer_db.GetSignerType();
  if (signer_type == SignerType::SOFTWARE) {
    if (signer_passphrase_.count(mid) == 0) {
      try {
        GetSignerDb(chain, mid).GetSoftwareSigner("");
        signer_passphrase_[mid] = "";
      } catch (...) {
      }
    }
    device.set_needs_pass_phrase_sent(signer_passphrase_.count(id) == 0);
  }
  MasterSigner signer{id, device, signer_db.GetLastHealthCheck(), signer_type};
  signer.set_name(signer_db.GetName());
  return signer;
}

SoftwareSigner NunchukStorage::GetSoftwareSigner(Chain chain,
                                                 const std::string& id) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  auto mid = ba::to_lower_copy(id);
  if (signer_passphrase_.count(mid) == 0) {
    auto software_signer = GetSignerDb(chain, mid).GetSoftwareSigner("");
    signer_passphrase_[mid] = "";
    return software_signer;
  }
  return GetSignerDb(chain, mid).GetSoftwareSigner(signer_passphrase_.at(mid));
}

bool NunchukStorage::UpdateWallet(Chain chain, const Wallet& wallet) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  auto wallet_db = GetWalletDb(chain, wallet.get_id());
  return wallet_db.SetName(wallet.get_name()) &&
         wallet_db.SetDescription(wallet.get_description());
}

bool NunchukStorage::UpdateMasterSigner(Chain chain,
                                        const MasterSigner& signer) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, signer.get_id()).SetName(signer.get_name());
}

bool NunchukStorage::DeleteWallet(Chain chain, const std::string& id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  GetWalletDb(chain, id).DeleteWallet();
  return fs::remove(GetWalletDir(chain, id));
}

bool NunchukStorage::DeleteMasterSigner(Chain chain, const std::string& id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  GetSignerDb(chain, id).DeleteSigner();
  return fs::remove(GetSignerDir(chain, id));
}

bool NunchukStorage::SetHealthCheckSuccess(Chain chain,
                                           const std::string& mastersigner_id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, mastersigner_id).SetLastHealthCheck(std::time(0));
}

bool NunchukStorage::SetHealthCheckSuccess(Chain chain,
                                           const SingleSigner& signer) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, signer.get_master_fingerprint())
      .SetRemoteLastHealthCheck(signer.get_derivation_path(), std::time(0));
}

bool NunchukStorage::AddAddress(Chain chain, const std::string& wallet_id,
                                const std::string& address, int index,
                                bool internal) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).AddAddress(address, index, internal);
}

bool NunchukStorage::UseAddress(Chain chain, const std::string& wallet_id,
                                const std::string& address) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).UseAddress(address);
}

std::vector<std::string> NunchukStorage::GetAddresses(
    Chain chain, const std::string& wallet_id, bool used, bool internal) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetAddresses(used, internal);
}

std::vector<std::string> NunchukStorage::GetAllAddresses(
    Chain chain, const std::string& wallet_id) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetAllAddresses();
}

int NunchukStorage::GetCurrentAddressIndex(Chain chain,
                                           const std::string& wallet_id,
                                           bool internal) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetCurrentAddressIndex(internal);
}

Transaction NunchukStorage::InsertTransaction(
    Chain chain, const std::string& wallet_id, const std::string& raw_tx,
    int height, time_t blocktime, Amount fee, const std::string& memo,
    int change_pos) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id)
      .InsertTransaction(raw_tx, height, blocktime, fee, memo, change_pos);
}

std::vector<Transaction> NunchukStorage::GetTransactions(
    Chain chain, const std::string& wallet_id, int count, int skip) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  auto db = GetWalletDb(chain, wallet_id);
  auto vtx = db.GetTransactions(count, skip);

  // remove invalid, out-of-date Send transactions
  auto utxos = db.GetUnspentOutputs(false);
  auto is_valid_input = [utxos](const TxInput& input) {
    for (auto&& utxo : utxos) {
      if (input.first == utxo.get_txid() && input.second == utxo.get_vout())
        return true;
    }
    return false;
  };
  auto end = std::remove_if(vtx.begin(), vtx.end(), [&](const Transaction& tx) {
    if (tx.get_height() == -1) {
      for (auto&& input : tx.get_inputs()) {
        if (!is_valid_input(input)) {
          return true;
        }
      }
    }
    return false;
  });
  vtx.erase(end, vtx.end());

  for (auto&& tx : vtx) {
    db.FillSendReceiveData(tx);
  }
  return vtx;
}

std::vector<UnspentOutput> NunchukStorage::GetUnspentOutputs(
    Chain chain, const std::string& wallet_id, bool remove_locked) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetUnspentOutputs(remove_locked);
}

Transaction NunchukStorage::GetTransaction(Chain chain,
                                           const std::string& wallet_id,
                                           const std::string& tx_id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  auto db = GetWalletDb(chain, wallet_id);
  auto tx = db.GetTransaction(tx_id);
  db.FillSendReceiveData(tx);
  return tx;
}

bool NunchukStorage::UpdateTransaction(Chain chain,
                                       const std::string& wallet_id,
                                       const std::string& raw_tx, int height,
                                       time_t blocktime,
                                       const std::string& reject_msg) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id)
      .UpdateTransaction(raw_tx, height, blocktime, reject_msg);
}

bool NunchukStorage::UpdateTransactionMemo(Chain chain,
                                           const std::string& wallet_id,
                                           const std::string& tx_id,
                                           const std::string& memo) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).UpdateTransactionMemo(tx_id, memo);
}

bool NunchukStorage::DeleteTransaction(Chain chain,
                                       const std::string& wallet_id,
                                       const std::string& tx_id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).DeleteTransaction(tx_id);
}

Transaction NunchukStorage::CreatePsbt(
    Chain chain, const std::string& wallet_id, const std::string& psbt,
    Amount fee, const std::string& memo, int change_pos,
    const std::map<std::string, Amount>& outputs, Amount fee_rate,
    bool subtract_fee_from_amount, const std::string& replace_tx) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id)
      .CreatePsbt(psbt, fee, memo, change_pos, outputs, fee_rate,
                  subtract_fee_from_amount, replace_tx);
}

bool NunchukStorage::UpdatePsbt(Chain chain, const std::string& wallet_id,
                                const std::string& psbt) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).UpdatePsbt(psbt);
}

bool NunchukStorage::UpdatePsbtTxId(Chain chain, const std::string& wallet_id,
                                    const std::string& old_id,
                                    const std::string& new_id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).UpdatePsbtTxId(old_id, new_id);
}

std::string NunchukStorage::GetPsbt(Chain chain, const std::string& wallet_id,
                                    const std::string& tx_id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetPsbt(tx_id);
}

bool NunchukStorage::SetUtxos(Chain chain, const std::string& wallet_id,
                              const std::string& address,
                              const std::string& utxo) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).SetUtxos(address, utxo);
}

Amount NunchukStorage::GetBalance(Chain chain, const std::string& wallet_id) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetBalance();
}
std::string NunchukStorage::FillPsbt(Chain chain, const std::string& wallet_id,
                                     const std::string& psbt) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).FillPsbt(psbt);
}

// non-reentrant function
void NunchukStorage::MaybeMigrate(Chain chain) {
  static std::once_flag flag;
  std::call_once(flag, [&] {
    auto wallets = ListWallets(chain);
    {
      boost::unique_lock<boost::shared_mutex> lock(access_);
      for (auto&& wallet_id : wallets) {
        GetWalletDb(chain, wallet_id).MaybeMigrate();
      }
    }

    // migrate app state
    auto appstate = GetAppStateDb(chain);
    int64_t current_ver = appstate.GetStorageVersion();
    if (current_ver == STORAGE_VER) return;
    if (current_ver < 3) {
      for (auto&& wallet_id : wallets) {
        GetWallet(chain, wallet_id, true);
      }
    }
    DLOG_F(INFO, "NunchukAppStateDb migrate to version %d", STORAGE_VER);
    appstate.SetStorageVersion(STORAGE_VER);
  });
}

int NunchukStorage::GetChainTip(Chain chain) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetAppStateDb(chain).GetChainTip();
}

bool NunchukStorage::SetChainTip(Chain chain, int value) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetAppStateDb(chain).SetChainTip(value);
}

std::string NunchukStorage::GetSelectedWallet(Chain chain) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetAppStateDb(chain).GetSelectedWallet();
}

bool NunchukStorage::SetSelectedWallet(Chain chain, const std::string& value) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetAppStateDb(chain).SetSelectedWallet(value);
}

std::vector<SingleSigner> NunchukStorage::GetRemoteSigners(Chain chain) {
  auto signers = ListMasterSigners(chain);
  boost::shared_lock<boost::shared_mutex> lock(access_);
  std::vector<SingleSigner> rs;
  for (auto&& signer_id : signers) {
    auto remote = GetSignerDb(chain, signer_id).GetRemoteSigners();
    rs.insert(rs.end(), remote.begin(), remote.end());
  }
  return rs;
}

bool NunchukStorage::DeleteRemoteSigner(Chain chain,
                                        const std::string& master_fingerprint,
                                        const std::string& derivation_path) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, master_fingerprint)
      .DeleteRemoteSigner(derivation_path);
}

bool NunchukStorage::UpdateRemoteSigner(Chain chain,
                                        const SingleSigner& remotesigner) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, remotesigner.get_master_fingerprint())
      .SetRemoteName(remotesigner.get_derivation_path(),
                     remotesigner.get_name());
}

bool NunchukStorage::IsMasterSigner(Chain chain, const std::string& id) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetSignerDb(chain, id).IsMaster();
}

int NunchukStorage::GetAddressIndex(Chain chain, const std::string& wallet_id,
                                    const std::string& address) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  int index = GetWalletDb(chain, wallet_id).GetAddressIndex(address);
  if (index < 0)
    throw StorageException(StorageException::ADDRESS_NOT_FOUND,
                           "address not found");
  return index;
}

Amount NunchukStorage::GetAddressBalance(Chain chain,
                                         const std::string& wallet_id,
                                         const std::string& address) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetAddressBalance(address);
}

std::string NunchukStorage::GetMultisigConfig(Chain chain,
                                              const std::string& wallet_id,
                                              bool is_cobo) {
  boost::shared_lock<boost::shared_mutex> lock(access_);
  return GetWalletDb(chain, wallet_id).GetMultisigConfig(is_cobo);
}

void NunchukStorage::SendSignerPassphrase(Chain chain,
                                          const std::string& mastersigner_id,
                                          const std::string& passphrase) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  GetSignerDb(chain, mastersigner_id).GetSoftwareSigner(passphrase);
  signer_passphrase_[ba::to_lower_copy(mastersigner_id)] = passphrase;
}

void NunchukStorage::ClearSignerPassphrase(Chain chain,
                                           const std::string& mastersigner_id) {
  boost::unique_lock<boost::shared_mutex> lock(access_);
  signer_passphrase_.erase(ba::to_lower_copy(mastersigner_id));
}

std::string NunchukStorage::ExportBackup() {
  boost::shared_lock<boost::shared_mutex> lock(access_);

  auto exportChain = [&](Chain chain) {
    json rs;
    rs["wallets"] = json::array();
    auto wids = ListWallets0(chain);
    for (auto&& id : wids) {
      auto wallet_db = GetWalletDb(chain, id);
      auto w = wallet_db.GetWallet();
      json wallet = {
          {"id", w.get_id()},
          {"name", w.get_name()},
          {"descriptor", w.get_descriptor(DescriptorPath::ANY)},
          {"create_date", w.get_create_date()},
          {"description", w.get_description()},
      };
      rs["wallets"].push_back(wallet);
    }

    rs["signers"] = json::array();
    auto sids = ListMasterSigners0(chain);
    for (auto&& id : sids) {
      auto signerDb = GetSignerDb(chain, id);
      if (signerDb.GetId().empty()) continue;
      json signer = {{"id", signerDb.GetId()},
                     {"name", signerDb.GetName()},
                     {"device_type", signerDb.GetDeviceType()},
                     {"device_model", signerDb.GetDeviceModel()},
                     {"last_health_check", signerDb.GetLastHealthCheck()},
                     {"bip32", json::array()},
                     {"remote", json::array()}};
      auto singleSigners = signerDb.GetSingleSigners(false);
      for (auto&& singleSigner : singleSigners) {
        signer["bip32"].push_back({{"path", singleSigner.get_derivation_path()},
                                   {"xpub", singleSigner.get_xpub()}});
      }
      auto remoteSigners = signerDb.GetRemoteSigners();
      for (auto&& singleSigner : remoteSigners) {
        signer["remote"].push_back(
            {{"path", singleSigner.get_derivation_path()},
             {"xpub", singleSigner.get_xpub()},
             {"pubkey", singleSigner.get_public_key()},
             {"name", singleSigner.get_name()},
             {"last_health_check", singleSigner.get_last_health_check()}});
      }
      rs["signers"].push_back(signer);
    }
    return rs;
  };

  json data = {{"testnet", exportChain(Chain::TESTNET)},
               {"mainnet", exportChain(Chain::MAIN)},
               {"ts", std::time(0)}};
  return data.dump();
}

bool NunchukStorage::SyncWithBackup(const std::string& dataStr,
                                    std::function<bool(int)> progress) {
  boost::unique_lock<boost::shared_mutex> lock(access_);

  int percent = 0;
  auto importChain = [&](Chain chain, const json& d) {
    json signers = d["signers"];
    for (auto&& signer : signers) {
      std::string id = signer["id"];
      if (id.empty()) continue;
      fs::path db_file = GetSignerDir(chain, id);
      NunchukSignerDb db{chain, id, db_file.string(), passphrase_};
      db.InitSigner(signer["name"],
                    {signer["device_type"], signer["device_model"], id}, "");
      db.SetLastHealthCheck(signer["last_health_check"]);
      for (auto&& ss : signer["bip32"]) {
        db.AddXPub(ss["path"], ss["xpub"], GetBip32Type(ss["path"]));
      }
      for (auto&& ss : signer["remote"]) {
        db.AddRemote(ss["name"], ss["xpub"], ss["pubkey"], ss["path"]);
        db.SetRemoteLastHealthCheck(ss["path"], ss["last_health_check"]);
      }
    }
    percent += 25;
    progress(percent);

    json wallets = d["wallets"];
    for (auto&& wallet : wallets) {
      std::string id = wallet["id"];
      if (id.empty()) continue;
      fs::path db_file = GetWalletDir(chain, id);
      if (!fs::exists(db_file)) {
        AddressType a;
        WalletType w;
        int m;
        int n;
        std::vector<SingleSigner> signers;
        if (ParseDescriptors(wallet["descriptor"], a, w, m, n, signers)) {
          CreateWallet0(chain, wallet["name"], m, n, signers, a,
                        w == WalletType::ESCROW, wallet["description"], true,
                        wallet["create_date"]);
        }
      } else {
        auto db = GetWalletDb(chain, id);
        db.SetName(wallet["name"]);
        db.SetDescription(wallet["description"]);
      }
    }
    percent += 25;
    progress(percent);
  };

  auto appState = GetAppStateDb(Chain::MAIN);
  json data = json::parse(dataStr);
  time_t ts = data["ts"];
  time_t lastSyncTs = appState.GetLastSyncTs();
  if (lastSyncTs > ts) {
    progress(100);
    return false;  // old backup
  }
  importChain(Chain::TESTNET, data["testnet"]);
  importChain(Chain::MAIN, data["mainnet"]);
  return appState.SetLastSyncTs(ts);
}

}  // namespace nunchuk