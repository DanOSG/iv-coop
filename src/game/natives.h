#pragma once
// Script native invoker for GTA IV 1.2.0.59.
//
// Natives must be called on the game thread from inside a script context (see
// script::on_tick); several of them (entity creation especially) attach results to the
// current script thread.
//
// Hash values are taken from the community native list (as used by FusionFix); IV hashes
// are not joaat of the name, so they can't be computed.

#include <cstdint>
#include <type_traits>

namespace ivc::natives {

using Ped = std::int32_t;
using Player = std::int32_t;
using Blip = std::int32_t;

struct Vector3 { float x, y, z; };
struct Vector4 { float x, y, z, w; };

// Matches rage::scrThread::Info.
struct CallContext {
  void* result;
  std::int32_t arg_count;
  void* args;
  std::int32_t vec_fixup_count;
  Vector3* vec_orig[4];
  Vector4 vec_tmp[4];
  std::uint8_t buf[16 * 4];
};

// Returns the handler for `hash`, cached; nullptr (and an error log, once) if unknown.
void* handler(std::uint32_t hash);
// False if the game layer failed to initialise; invocations then become no-ops.
bool available();

template <class R = void, class... Args>
R invoke(std::uint32_t hash, Args... args) {
  static_assert(((sizeof(Args) <= 4) && ...), "native arguments must be 32-bit or smaller");
  CallContext ctx{};
  ctx.args = ctx.result = ctx.buf;
  ((reinterpret_cast<std::uint32_t*>(ctx.buf)[ctx.arg_count] = 0,
    *reinterpret_cast<Args*>(ctx.buf + 4 * ctx.arg_count++) = args), ...);

  if (auto* fn = reinterpret_cast<void(__cdecl*)(CallContext*)>(handler(hash))) {
    fn(&ctx);
    while (ctx.vec_fixup_count > 0) {
      --ctx.vec_fixup_count;
      const Vector4& t = ctx.vec_tmp[ctx.vec_fixup_count];
      *ctx.vec_orig[ctx.vec_fixup_count] = {t.x, t.y, t.z};
    }
  }
  if constexpr (!std::is_void_v<R>) return *reinterpret_cast<R*>(ctx.buf);
}

// --- typed wrappers (add as needed) ---
inline Player GetPlayerId() { return invoke<Player>(0x62E319C6); }
inline bool IsPlayerPlaying(Player p) { return invoke<bool>(0x08274BA4, p); }
inline Ped GetPlayerChar(Player p) { Ped ped = 0; invoke(0x511454A9, p, &ped); return ped; }
inline bool DoesCharExist(Ped ped) { return invoke<bool>(0x46531797, ped); }
inline Vector3 GetCharCoordinates(Ped ped) { Vector3 v{}; invoke(0x2B5C06E6, ped, &v.x, &v.y, &v.z); return v; }
inline float GetCharHeading(Ped ped) { float h = 0; invoke(0x057A3AC7, ped, &h); return h; }
inline Vector3 GetCharVelocity(Ped ped) { Vector3 v{}; invoke(0x3B977FD4, ped, &v.x, &v.y, &v.z); return v; }
inline std::uint32_t GetCharModel(Ped ped) { std::uint32_t m = 0; invoke(0x0A3D60CE, ped, &m); return m; }
inline bool IsCharInAnyCar(Ped ped) { return invoke<bool>(0x71184DA3, ped); }
inline std::uint32_t GetGameTimer() { std::uint32_t t = 0; invoke(0x022B2DA9, &t); return t; }
inline std::uint32_t GetHashKey(const char* s) { return invoke<std::uint32_t>(0x68FF7165, s); }

inline void RequestModel(std::uint32_t model) { invoke(0x502B5185, model); }
inline bool HasModelLoaded(std::uint32_t model) { return invoke<bool>(0x4E61480A, model); }
inline void MarkModelAsNoLongerNeeded(std::uint32_t model) { invoke(0x00FA0E33, model); }

inline Ped CreateChar(std::int32_t ped_type, std::uint32_t model, Vector3 at) {
  Ped ped = 0;
  invoke(0x4A673763, ped_type, model, at.x, at.y, at.z, &ped, true);
  return ped;
}
inline void DeleteChar(Ped& ped) { invoke(0x0E3B49BF, &ped); }
inline void SetCharCoordinatesNoOffset(Ped ped, Vector3 v) { invoke(0x57C758F0, ped, v.x, v.y, v.z); }
inline void SetCharHeading(Ped ped, float h) { invoke(0x46B5523B, ped, h); }
inline void SetBlockingOfNonTemporaryEvents(Ped ped, bool b) { invoke(0x76247429, ped, b); }
inline void SetCharInvincible(Ped ped, bool b) { invoke(0x2A58578B, ped, b); }
inline void FreezeCharPosition(Ped ped, bool b) { invoke(0x20266A86, ped, b); }
inline void SetCharCollision(Ped ped, bool b) { invoke(0x2A7413EB, ped, b); }
inline Blip AddBlipForChar(Ped ped) { Blip b = 0; invoke(0x19A64C5D, ped, &b); return b; }
inline void RemoveBlip(Blip b) { invoke(0x7BBF3625, b); }

// Movement / tasks.
enum class MoveState : std::uint32_t { Walk = 2, Run = 3, Sprint = 4 };
inline void TaskGoStraightToCoord(Ped ped, Vector3 to, MoveState ms, std::uint32_t timeout_ms = 45000) {
  invoke(0x19591255, ped, to.x, to.y, to.z, static_cast<std::uint32_t>(ms), timeout_ms);
}
inline void TaskAchieveHeading(Ped ped, float heading) { invoke(0x6D6A1261, ped, heading); }
inline void ClearCharTasks(Ped ped) { invoke(0x4AB470F3, ped); }
inline void SetCharMoveAnimSpeedMultiplier(Ped ped, float m) { invoke(0x5DC456DE, ped, m); }
inline void SetCharKeepTask(Ped ped, bool b) { invoke(0x264009D3, ped, b); }
inline void SetPedDontDoEvasiveDives(Ped ped, bool b) { invoke(0x1EAD1D7D, ped, b); }
inline void SetDontActivateRagdollFromPlayerImpact(Ped ped, bool b) { invoke(0x5A676BCD, ped, b); }
inline void SetCharWillMoveWhenInjured(Ped ped, bool b) { invoke(0x1EF36397, ped, b); }

// Vehicles.
using Vehicle = std::int32_t;
inline constexpr std::int32_t kDriverSeat = -1;
inline bool DoesVehicleExist(Vehicle v) { return invoke<bool>(0x67A42263, v); }
inline Vehicle StoreCarCharIsInNoSave(Ped ped) { Vehicle v = 0; invoke(0x21CC647F, ped, &v); return v; }
inline std::uint32_t GetCarModel(Vehicle v) { std::uint32_t m = 0; invoke(0x5FF84497, v, &m); return m; }
inline Vector3 GetCarCoordinates(Vehicle v) { Vector3 p{}; invoke(0x2D432EAB, v, &p.x, &p.y, &p.z); return p; }
inline Vector4 GetVehicleQuaternion(Vehicle v) { Vector4 q{}; invoke(0x6C5871D6, v, &q.x, &q.y, &q.z, &q.w); return q; }
inline void SetVehicleQuaternion(Vehicle v, Vector4 q) { invoke(0x43573596, v, q.x, q.y, q.z, q.w); }
inline float GetCarHeading(Vehicle v) { float h = 0; invoke(0x46803CFA, v, &h); return h; }
inline Vector3 GetCarSpeedVector(Vehicle v) { Vector3 s{}; invoke(0x112E7FB1, v, &s, false); return s; }
inline float GetCarSpeed(Vehicle v) { float s = 0; invoke(0x16DD2D00, v, &s); return s; }
inline void SetCarCoordinatesNoOffset(Vehicle v, Vector3 p) { invoke(0x12D64378, v, p.x, p.y, p.z); }
inline void SetCarForwardSpeed(Vehicle v, float s) { invoke(0x65BB0060, v, s); }
inline Vehicle CreateCar(std::uint32_t model, Vector3 at) { Vehicle v = 0; invoke(0x2F1D6843, model, at.x, at.y, at.z, &v, true); return v; }
inline void DeleteCar(Vehicle& v) { invoke(0x7F71342D, &v); }
inline void WarpCharIntoCar(Ped ped, Vehicle v) { invoke(0x73D3504A, ped, v); }
inline void WarpCharIntoCarAsPassenger(Ped ped, Vehicle v, std::uint32_t seat) { invoke(0x172376FE, ped, v, seat); }
inline void WarpCharFromCarToCoord(Ped ped, Vector3 p) { invoke(0x6A77506A, ped, p.x, p.y, p.z); }
inline bool IsCharInCar(Ped ped, Vehicle v) { return invoke<bool>(0x7D037B40, ped, v); }
inline Ped GetDriverOfCar(Vehicle v) { Ped p = 0; invoke(0x22457083, v, &p); return p; }
inline Ped GetCharInCarPassengerSeat(Vehicle v, std::uint32_t seat) { Ped p = 0; invoke(0x5E756B51, v, seat, &p); return p; }
inline bool IsCarPassengerSeatFree(Vehicle v, std::uint32_t seat) { return invoke<bool>(0x1BDA0DA5, v, seat); }
inline std::uint32_t GetMaximumNumberOfPassengers(Vehicle v) { std::uint32_t n = 0; invoke(0x554014F1, v, &n); return n; }
inline void GetCarColours(Vehicle v, std::int32_t& c1, std::int32_t& c2) { invoke(0x6CAC3D62, v, &c1, &c2); }
inline void ChangeCarColour(Vehicle v, std::int32_t c1, std::int32_t c2) { invoke(0x06441EAF, v, c1, c2); }
inline void GetExtraCarColours(Vehicle v, std::int32_t& c1, std::int32_t& c2) { invoke(0x25B87BCA, v, &c1, &c2); }
inline void SetExtraCarColours(Vehicle v, std::int32_t c1, std::int32_t c2) { invoke(0x6CB14354, v, c1, c2); }
inline std::uint32_t GetCarHealth(Vehicle v) { std::uint32_t h = 0; invoke(0x4D417CD3, v, &h); return h; }
inline void SetCarHealth(Vehicle v, std::uint32_t h) { invoke(0x49B6525C, v, h); }
inline float GetEngineHealth(Vehicle v) { return invoke<float>(0x2B0A05E0, v); }
inline void SetEngineHealth(Vehicle v, float h) { invoke(0x3F413561, v, h); }
inline bool IsCarSirenOn(Vehicle v) { return invoke<bool>(0x129A1569, v); }
inline void SwitchCarSiren(Vehicle v, bool on) { invoke(0x7781290F, v, on); }
inline void SetCarEngineOn(Vehicle v, bool on, bool instantly) { invoke(0x0CAA42D0, v, on, instantly); }
inline void SetCarAsMissionCar(Vehicle v) { invoke(0x210A33B2, v); }
inline void SetCarCanBeDamaged(Vehicle v, bool b) { invoke(0x394E733E, v, b); }

// Ped types for CREATE_CHAR.
inline constexpr std::int32_t kPedTypeCivMale = 4;

} // namespace ivc::natives
