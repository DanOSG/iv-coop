#include "story/sounds.h"

#include <cctype>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "game/native_hooks.h"
#include "net/bytes.h"
#include "story/story_sync.h"

namespace ivc::story::sounds {
namespace {
namespace n = natives;
using n::invoke;
using native_hooks::Call;

constexpr std::uint32_t kPlayPos = 0x65752C65, kPlayPed = 0x56F37A81, kPlayVeh = 0x763274B7, kPlayObj = 0x60AE0867,
                        kPlayFrontend = 0x4DAF2C87, kPlay = 0x47CA7C53, kEvent = 0x486F3D93, kEventPed = 0x61064783,
                        kEventVeh = 0x2F4B2A8B, kEventObj = 0x4BB9178A, kFire = 0x4B6135E8,
                        kMissionComplete = 0x4BAF0213, kPreload = 0x39DE515D, kPreloadOffset = 0x2B8836A6,
                        kStreamFrontend = 0x133C257F, kStreamObject = 0x4AA86394, kStreamPed = 0x0C47057F,
                        kStopStream = 0x66915CE9, kStopSound = 0x09DB00B9, kReleaseId = 0x211D390A,
                        kGetId = 0x6342018A;

enum class Op : std::uint8_t {
  Position, Ped, Vehicle, Object, // PLAY_SOUND_FROM_*
  Frontend,                       // PLAY_SOUND_FRONTEND / PLAY_SOUND
  Event, EventPed, EventVehicle, EventObject,
  Fire,            // PLAY_FIRE_SOUND_FROM_POSITION
  MissionComplete, // arg = audio id
  Preload,         // arg = start offset
  StreamFrontend, StreamObject, StreamPed, StopStream,
  Stop, Release, // sound id only
};

enum class Who : std::uint8_t { None, HostPlayer, Entity };

struct Event {
  Op op = Op::Position;
  Who who = Who::None;
  std::int32_t sound = -1; // the host's sound id
  std::int32_t arg = 0;
  std::uint32_t entity = 0;
  n::Vector3 pos{};
  std::array<char, 64> name{};
};

// ---------------------------------------------------------------- host capture

struct Pending {
  Event ev;
  n::Handle handle = 0; // the script's entity handle, resolved when sending
};

bool g_hosting = false;
std::deque<Pending> g_out;
std::string g_last_preload; // scripts poll PRELOAD_STREAM every frame; send it once

// The host's phone, wardrobe, internet and other UI scripts play their own feedback sounds (dial tones,
// ring, hang-up); those stay on the host.
bool ui_script(const std::string& script) {
  static const char* kPrefix[] = {"spcellphone", "ambwardrobe", "ambinternetcafe", "policecomputer", "ambtv"};
  std::string lower;
  for (const char ch : script) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  for (const char* p : kPrefix)
    if (lower.rfind(p, 0) == 0) return true;
  return false;
}

bool capture(const Call& c) { return g_hosting && is_story_script(c.script) && !ui_script(c.script); }

// UI feedback of the host's own menus; kept on the host.
bool ui_sound(const std::string& name) {
  static const char* kPrefix[] = {"POLICE_COMPUTER", "PC_", "PHONE_", "MOBILE_PHONE", "WEB_", "FRONTEND_MENU",
                                  "WARDROBE", "MENU_", "FRONTEND_GAME_PHONE"};
  for (const char* p : kPrefix)
    if (name.rfind(p, 0) == 0) return true;
  return false;
}

void set_name(Event& e, const std::string& s) {
  std::memcpy(e.name.data(), s.data(), std::min(s.size(), e.name.size() - 1));
}

// Queue an event. An arg index < 0 means that argument is absent; pos_arg is the first of 3 floats.
bool push(const Call& c, Op op, int sound_arg, int name_arg, int ent_arg, int pos_arg = -1, bool frontend = false) {
  if (!capture(c)) return false;
  Pending p;
  p.ev.op = op;
  if (sound_arg >= 0) p.ev.sound = c.arg<std::int32_t>(sound_arg);
  if (name_arg >= 0) {
    const std::string name = c.str(name_arg);
    if (name.empty() || name[0] == '(') return false; // unreadable
    if (frontend && ui_sound(name)) return false;
    set_name(p.ev, name);
  }
  if (pos_arg >= 0) p.ev.pos = {c.arg<float>(pos_arg), c.arg<float>(pos_arg + 1), c.arg<float>(pos_arg + 2)};
  if (ent_arg >= 0) {
    p.handle = c.arg<n::Handle>(ent_arg);
    if (!p.handle) return false;
    // Where it is now: the joiner's fallback if the entity has no copy there.
    switch (op) {
      case Op::Ped: case Op::EventPed: case Op::StreamPed:
        if (n::DoesCharExist(p.handle)) p.ev.pos = n::GetCharCoordinates(p.handle);
        break;
      case Op::Vehicle: case Op::EventVehicle:
        if (n::DoesVehicleExist(p.handle)) p.ev.pos = n::GetCarCoordinates(p.handle);
        break;
      default:
        if (n::DoesObjectExist(p.handle)) p.ev.pos = n::GetObjectCoordinates(p.handle);
        break;
    }
  }
  g_out.push_back(p);
  return true;
}

// ---------------------------------------------------------------- client playback

std::function<n::Ped()> g_host_ped;
std::unordered_map<std::int32_t, std::int32_t> g_ids; // host sound id -> local sound id
std::string g_preloaded;                              // the stream being loaded
std::int32_t g_preload_offset = 0;
bool g_stream_loaded = false;
Op g_stream_play = Op::Stop; // a stream start waiting for the load (Stop = none)
n::Handle g_stream_entity = 0;

std::int32_t local_sound(std::int32_t host_id, bool create) {
  if (host_id < 0) return -1;
  if (const auto it = g_ids.find(host_id); it != g_ids.end()) return it->second;
  if (!create) return -1;
  const auto id = invoke<std::int32_t>(kGetId);
  g_ids[host_id] = id;
  return id;
}

// The local copy for an event's entity, or 0.
n::Handle local_entity(const Event& e, sync::EntitySync& ents) {
  if (e.who == Who::HostPlayer) {
    const n::Ped host = g_host_ped ? g_host_ped() : 0;
    return host && n::DoesCharExist(host) ? host : 0;
  }
  if (e.who == Who::Entity) return ents.handle_for_id(e.entity);
  return 0;
}

void start_stream(Op op, n::Handle ent) {
  if (op == Op::StreamFrontend) invoke(kStreamFrontend);
  else if (op == Op::StreamObject && n::DoesObjectExist(ent)) invoke(kStreamObject, ent);
  else if (op == Op::StreamPed && n::DoesCharExist(ent)) invoke(kStreamPed, ent);
}

void play(const Event& e, sync::EntitySync& ents) {
  const std::string name(e.name.data(), strnlen(e.name.data(), e.name.size()));
  const char* nm = name.c_str();
  const n::Handle ent = local_entity(e, ents);
  const bool have_pos = e.pos.x != 0 || e.pos.y != 0 || e.pos.z != 0;
  switch (e.op) {
    case Op::Position:
      invoke(kPlayPos, local_sound(e.sound, true), nm, e.pos.x, e.pos.y, e.pos.z);
      break;
    case Op::Ped: case Op::Vehicle: case Op::Object: {
      const std::int32_t id = local_sound(e.sound, true);
      const bool ok = ent && (e.op == Op::Ped ? n::DoesCharExist(ent)
                              : e.op == Op::Vehicle ? n::DoesVehicleExist(ent) : n::DoesObjectExist(ent));
      if (ok) invoke(e.op == Op::Ped ? kPlayPed : e.op == Op::Vehicle ? kPlayVeh : kPlayObj, id, nm, ent);
      else if (have_pos) invoke(kPlayPos, id, nm, e.pos.x, e.pos.y, e.pos.z);
      break;
    }
    case Op::Frontend:
      invoke(kPlayFrontend, local_sound(e.sound, true), nm);
      break;
    case Op::Event:
      invoke(kEvent, nm);
      break;
    case Op::EventPed: case Op::EventVehicle: case Op::EventObject: {
      const bool ok = ent && (e.op == Op::EventPed ? n::DoesCharExist(ent)
                              : e.op == Op::EventVehicle ? n::DoesVehicleExist(ent) : n::DoesObjectExist(ent));
      if (ok) invoke(e.op == Op::EventPed ? kEventPed : e.op == Op::EventVehicle ? kEventVeh : kEventObj, nm, ent);
      break;
    }
    case Op::Fire:
      invoke(kFire, local_sound(e.sound, true), e.pos.x, e.pos.y, e.pos.z);
      break;
    case Op::MissionComplete:
      invoke(kMissionComplete, e.arg);
      break;
    case Op::Preload:
      g_preloaded = name, g_preload_offset = e.arg, g_stream_loaded = false, g_stream_play = Op::Stop;
      break;
    case Op::StreamFrontend: case Op::StreamObject: case Op::StreamPed:
      if (g_stream_loaded) start_stream(e.op, ent);
      else if (!g_preloaded.empty()) g_stream_play = e.op, g_stream_entity = ent; // starts when loaded
      break;
    case Op::StopStream:
      invoke(kStopStream);
      g_preloaded.clear(), g_stream_loaded = false, g_stream_play = Op::Stop;
      break;
    case Op::Stop:
      if (const std::int32_t id = local_sound(e.sound, false); id >= 0) invoke(kStopSound, id);
      break;
    case Op::Release:
      if (const auto it = g_ids.find(e.sound); it != g_ids.end()) {
        invoke(kStopSound, it->second);
        invoke(kReleaseId, it->second);
        g_ids.erase(it);
      }
      break;
  }
}

} // namespace

void register_hooks() {
  using native_hooks::on;
  on(kPlayPos, "PLAY_SOUND_FROM_POSITION", [](const Call& c) { push(c, Op::Position, 0, 1, -1, 2); });
  on(kPlayPed, "PLAY_SOUND_FROM_PED", [](const Call& c) { push(c, Op::Ped, 0, 1, 2); });
  on(kPlayVeh, "PLAY_SOUND_FROM_VEHICLE", [](const Call& c) { push(c, Op::Vehicle, 0, 1, 2); });
  on(kPlayObj, "PLAY_SOUND_FROM_OBJECT", [](const Call& c) { push(c, Op::Object, 0, 1, 2); });
  on(kPlayFrontend, "PLAY_SOUND_FRONTEND", [](const Call& c) { push(c, Op::Frontend, 0, 1, -1, -1, true); });
  on(kPlay, "PLAY_SOUND", [](const Call& c) { push(c, Op::Frontend, 0, 1, -1, -1, true); });
  on(kEvent, "PLAY_AUDIO_EVENT", [](const Call& c) { push(c, Op::Event, -1, 0, -1, -1, true); });
  on(kEventPed, "PLAY_AUDIO_EVENT_FROM_PED", [](const Call& c) { push(c, Op::EventPed, -1, 0, 1); });
  on(kEventVeh, "PLAY_AUDIO_EVENT_FROM_VEHICLE", [](const Call& c) { push(c, Op::EventVehicle, -1, 0, 1); });
  on(kEventObj, "PLAY_AUDIO_EVENT_FROM_OBJECT", [](const Call& c) { push(c, Op::EventObject, -1, 0, 1); });
  on(kFire, "PLAY_FIRE_SOUND_FROM_POSITION", [](const Call& c) { push(c, Op::Fire, 0, -1, -1, 1); });
  on(kMissionComplete, "TRIGGER_MISSION_COMPLETE_AUDIO", [](const Call& c) {
    if (!capture(c)) return;
    Pending p;
    p.ev.op = Op::MissionComplete, p.ev.arg = c.arg<std::int32_t>(0);
    g_out.push_back(p);
  });
  on(kPreload, "PRELOAD_STREAM", [](const Call& c) {
    if (capture(c) && c.str(0) == g_last_preload) return;
    if (push(c, Op::Preload, -1, 0, -1)) g_last_preload = c.str(0);
  });
  on(kPreloadOffset, "PRELOAD_STREAM_WITH_START_OFFSET", [](const Call& c) {
    if (capture(c) && c.str(0) == g_last_preload) return;
    if (push(c, Op::Preload, -1, 0, -1)) g_out.back().ev.arg = c.arg<std::int32_t>(1), g_last_preload = c.str(0);
  });
  on(kStreamFrontend, "PLAY_STREAM_FRONTEND", [](const Call& c) { push(c, Op::StreamFrontend, -1, -1, -1); });
  on(kStreamObject, "PLAY_STREAM_FROM_OBJECT", [](const Call& c) { push(c, Op::StreamObject, -1, -1, 0); });
  on(kStreamPed, "PLAY_STREAM_FROM_PED", [](const Call& c) { push(c, Op::StreamPed, -1, -1, 0); });
  on(kStopStream, "STOP_STREAM", [](const Call& c) {
    if (push(c, Op::StopStream, -1, -1, -1)) g_last_preload.clear();
  });
  on(kStopSound, "STOP_SOUND", [](const Call& c) { push(c, Op::Stop, 0, -1, -1); });
  on(kReleaseId, "RELEASE_SOUND_ID", [](const Call& c) { push(c, Op::Release, 0, -1, -1); });
  // GET_SOUND_ID needs no hook: a joiner allocates its own id lazily on first use of a host id.
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  if (!hosting) {
    g_out.clear();
    g_last_preload.clear();
    return;
  }
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  while (!g_out.empty()) {
    Pending p = std::move(g_out.front());
    g_out.pop_front();
    if (p.handle) {
      if (p.handle == me) p.ev.who = Who::HostPlayer;
      else if (const auto id = ents.id_for_handle(p.handle)) p.ev.who = Who::Entity, p.ev.entity = *id;
    }
    net::Writer w;
    w.put(kMsg).put(std::uint8_t{0}).put(p.ev);
    net.send_game(std::move(w.data()), true);
  }
}

void client_apply(std::span<const std::uint8_t> msg, sync::EntitySync& ents) {
  net::Reader r(msg);
  std::uint8_t type, sender;
  Event e;
  if (!r.get(type) || !r.get(sender) || !r.get(e)) return;
  e.name.back() = 0;
  play(e, ents);
}

void client_tick() {
  if (g_preloaded.empty() || g_stream_loaded) return;
  const bool ok = g_preload_offset > 0 ? invoke<bool>(kPreloadOffset, g_preloaded.c_str(), g_preload_offset)
                                       : invoke<bool>(kPreload, g_preloaded.c_str());
  if (!ok) return;
  g_stream_loaded = true;
  if (g_stream_play != Op::Stop) start_stream(g_stream_play, g_stream_entity);
  g_stream_play = Op::Stop;
}

void set_host_ped_provider(std::function<n::Ped()> provider) { g_host_ped = std::move(provider); }

void clear() {
  g_out.clear();
  for (const auto& [host, local] : g_ids) {
    invoke(kStopSound, local);
    invoke(kReleaseId, local);
  }
  g_ids.clear();
  if (!g_preloaded.empty()) invoke(kStopStream);
  g_preloaded.clear(), g_stream_loaded = false, g_stream_play = Op::Stop;
}

} // namespace ivc::story::sounds
