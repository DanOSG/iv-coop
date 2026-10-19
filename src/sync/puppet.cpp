#include "sync/puppet.h"

#include "core/log.h"

namespace ivc::sync {
namespace n = natives;

bool Puppet::ensure(const n::Vector3& at) {
  if (ped_ && n::DoesCharExist(ped_)) return true;
  if (ped_) {
    log::warn("Puppet ped {} vanished; respawning", ped_);
    ped_ = 0;
    blip_ = 0; // the game removes blips attached to deleted peds
  }
  if (!model_) model_ = n::GetHashKey(model_name_.c_str());
  n::RequestModel(model_);
  if (!n::HasModelLoaded(model_)) {
    phase_ = "loading model";
    return false;
  }
  ped_ = n::CreateChar(n::kPedTypeCivMale, model_, at);
  n::MarkModelAsNoLongerNeeded(model_);
  if (!ped_) {
    phase_ = "CREATE_CHAR failed";
    return false;
  }
  n::SetBlockingOfNonTemporaryEvents(ped_, true);
  n::SetCharInvincible(ped_, true);
  n::SetCharCollision(ped_, false);
  n::FreezeCharPosition(ped_, true);
  blip_ = n::AddBlipForChar(ped_);
  phase_ = "spawned";
  log::info("Puppet spawned: ped {} model {} (0x{:08X})", ped_, model_name_, model_);
  return true;
}

void Puppet::place(const n::Vector3& at, float heading) {
  if (!ped_) return;
  n::SetCharCoordinatesNoOffset(ped_, at);
  n::SetCharHeading(ped_, heading);
}

void Puppet::despawn() {
  if (blip_) n::RemoveBlip(blip_), blip_ = 0;
  if (ped_ && n::DoesCharExist(ped_)) n::DeleteChar(ped_);
  ped_ = 0;
  phase_ = "idle";
}

} // namespace ivc::sync
