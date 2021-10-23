// Copyright (c) 2020 Enigmo
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <nunchuk.h>
#include <vector>

namespace nunchuk {

MasterSigner::MasterSigner() {}
MasterSigner::MasterSigner(const std::string& id, const Device& device,
                           time_t last_health_check, SignerType signer_type)
    : id_(id),
      device_(device),
      last_health_check_(last_health_check),
      type_(signer_type) {}

std::string MasterSigner::get_id() const { return id_; }
std::string MasterSigner::get_name() const { return name_; }
Device MasterSigner::get_device() const { return device_; }
time_t MasterSigner::get_last_health_check() const {
  return last_health_check_;
}
bool MasterSigner::is_software() const {
  return type_ == SignerType::SOFTWARE || type_ == SignerType::FOREIGN_SOFTWARE;
}
SignerType MasterSigner::get_type() const { return type_; }

void MasterSigner::set_name(const std::string& value) { name_ = value; }

}  // namespace nunchuk