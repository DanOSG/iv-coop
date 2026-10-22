#include "story/story_sync.h"

#include <windows.h>

#include <deque>
#include <optional>
#include <map>
#include <set>
#include <variant>

#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "story/story_protocol.h"
#include "story/cutscene.h"
#include "story/dialogue.h"
#include "story/world_state.h"

namespace ivc::story {
namespace {
namespace n = natives;
using native_hooks::Call;

Settings g_cfg;
bool g_hosting = false; // set each frame by host_tick; capture is off otherwise
Status g_status;

// ---------------------------------------------------------------- host capture

struct SendOp { std::vector<std::uint8_t> bytes; };
struct RegisterOp { n::Handle handle; sync::EntityType type; std::uint32_t model; };
struct UnregisterOp { n::Handle handle; };
struct EntityBlipOp { std::uint32_t blip; n::Handle handle; };
using Op = std::variant<SendOp, RegisterOp, UnregisterOp, EntityBlipOp>;

std::deque<Op> g_ops;               // processed in order by host_tick
// The host's replicated story state, kept so a late joiner can be sent all of it.
// Entity blips are recorded when captured (so props and removal right after creation
// aren't lost) and `resolved` once host_tick knows the entity's network id.
struct HostBlip {
  BlipAdd add;
  bool resolved = false;
  std::map<BlipPropKind, BlipProp> props; // latest value of each
};
std::map<std::uint32_t, HostBlip> g_host_blips;
std::map<std::uint8_t, std::string> g_host_text_blocks; // slot -> block name

bool wildcard(const std::string& pat, const std::string& s) {
  // '*' matches any run; everything else literal. Small inputs: simple recursion.
  if (pat.empty()) return s.empty();
  if (pat[0] == '*') return wildcard(pat.substr(1), s) || (!s.empty() && wildcard(pat, s.substr(1)));
  return !s.empty() && pat[0] == s[0] && wildcard(pat.substr(1), s.substr(1));
}

bool capture(const Call& c) { return g_hosting && g_cfg.replicate && is_story_script(c.script); }

void send(std::vector<std::uint8_t> bytes) { g_ops.push_back(SendOp{std::move(bytes)}); }

void text(const Call& c, TextKind kind, int key_arg, int key2_arg, int number_arg, int time_arg, int flag_arg) {
  if (!capture(c)) return;
  Text t;
  t.kind = kind;
  if (key_arg >= 0) t.key = make_key(c.str(key_arg));
  if (key2_arg >= 0) t.key2 = make_key(c.str(key2_arg));
  if (number_arg >= 0) t.number = c.arg<std::int32_t>(number_arg);
  if (time_arg >= 0) t.time_ms = c.arg<std::uint32_t>(time_arg);
  if (flag_arg >= 0) t.flag = static_cast<std::uint8_t>(c.arg<std::uint32_t>(flag_arg) != 0);
  send(encode(t));
  ++g_status.texts;
}

void text_block(const Call& c, int name_arg, int slot_arg) {
  if (!capture(c)) return;
  TextBlock b;
  b.slot = static_cast<std::uint8_t>(c.arg<std::uint32_t>(slot_arg));
  if (name_arg >= 0) b.name = make_key(c.str(name_arg));
  if (name_arg >= 0)
    g_host_text_blocks[b.slot] = to_string(b.name);
  else
    g_host_text_blocks.erase(b.slot);
  send(encode(b));
}

// Mission payouts: a story script paying the protagonist. Purchases are ADD_SCORE too,
// but negative and from shop scripts, which run locally on every machine anyway; zero
// shows up for $0 rewards and owned items.
void payout(const Call& c) {
  if (!capture(c)) return;
  const auto amount = c.arg<std::int32_t>(1);
  if (c.arg<std::int32_t>(0) != n::GetPlayerId() || amount <= 0) return;
  log::info("Story: payout ${} from {}", amount, c.script);
  send(encode_payout(amount));
}

void blip_coord(const Call& c, BlipKind kind) {
  if (!capture(c)) return;
  BlipAdd b;
  b.id = static_cast<std::uint32_t>(c.out<std::int32_t>(3));
  b.kind = kind, b.x = c.arg<float>(0), b.y = c.arg<float>(1), b.z = c.arg<float>(2);
  if (!b.id) return;
  g_host_blips[b.id] = HostBlip{b, true, {}};
  send(encode(b));
}

void blip_prop(const Call& c, BlipPropKind prop) {
  if (!capture(c)) return;
  BlipProp p;
  p.id = c.arg<std::uint32_t>(0);
  const auto hb = g_host_blips.find(p.id);
  if (hb == g_host_blips.end()) return;
  p.prop = prop;
  if (prop == BlipPropKind::Scale)
    p.fvalue = c.arg<float>(1);
  else if (prop == BlipPropKind::Name)
    p.name = make_key(c.str(1));
  else
    p.ivalue = c.arg<std::int32_t>(1);
  hb->second.props[prop] = p;
  send(encode(p));
}

void entity_created(const Call& c, sync::EntityType type, std::uint32_t model, int out_arg) {
  if (!capture(c)) return;
  const n::Handle h = c.out<n::Handle>(out_arg);
  if (h) g_ops.push_back(RegisterOp{h, type, model});
}

void entity_released(const Call& c) {
  if (!capture(c)) return;
  // Runs *before* the native zeroes the handle. The pointer is into script memory; the
  // SEH guard around callbacks covers the unexpected.
  if (const auto* p = c.arg<const n::Handle*>(0); p && *p) g_ops.push_back(UnregisterOp{*p});
}

// ---------------------------------------------------------------- client playback

struct ClientBlip {
  n::Blip local = 0;
  BlipAdd add;
  std::vector<BlipProp> props; // applied on creation, and live afterwards
  DWORD since = GetTickCount();
};
std::map<std::uint32_t, ClientBlip> g_client_blips;

bool same_place(const BlipAdd& a, const BlipAdd& b) {
  return a.kind == b.kind && a.entity == b.entity && a.x == b.x && a.y == b.y && a.z == b.z;
}

// Text blocks the host loaded into slots on our side, and lines held back until they load
// (a PRINT for a key whose block isn't in yet shows nothing).
std::set<std::uint8_t> g_loading_slots;
std::map<std::uint8_t, std::string> g_loaded_slots; // slot -> block we requested there
std::deque<Text> g_held_texts;

void apply_prop(n::Blip b, const BlipProp& p) {
  switch (p.prop) {
    case BlipPropKind::Colour: n::ChangeBlipColour(b, p.ivalue); break;
    case BlipPropKind::Sprite: n::ChangeBlipSprite(b, static_cast<std::uint32_t>(p.ivalue)); break;
    case BlipPropKind::Display: n::ChangeBlipDisplay(b, static_cast<std::uint32_t>(p.ivalue)); break;
    case BlipPropKind::Route: n::SetRoute(b, p.ivalue != 0); break;
    case BlipPropKind::Scale: n::ChangeBlipScale(b, p.fvalue); break;
    case BlipPropKind::Name: n::ChangeBlipNameFromTextFile(b, to_string(p.name).c_str()); break;
  }
}

bool try_create(ClientBlip& cb, sync::EntitySync& ents) {
  const BlipAdd& a = cb.add;
  if (a.kind == BlipKind::Coord) {
    cb.local = n::AddBlipForCoord({a.x, a.y, a.z});
  } else if (a.kind == BlipKind::Contact) {
    cb.local = n::AddBlipForContact({a.x, a.y, a.z});
  } else {
    const n::Handle h = ents.handle_for_id(a.entity);
    if (!h) return false; // entity copy not spawned yet: retry next frame
    cb.local = n::DoesCharExist(h) ? n::AddBlipForChar(h) : n::AddBlipForCar(h);
  }
  if (!cb.local) return false;
  for (const auto& p : cb.props) apply_prop(cb.local, p);
  return true;
}

void apply_text(const Text& t) {
  const std::string k = to_string(t.key), k2 = to_string(t.key2);
  switch (t.kind) {
    case TextKind::Print: n::Print(k.c_str(), t.time_ms, t.flag); break;
    case TextKind::PrintNow: n::PrintNow(k.c_str(), t.time_ms, t.flag); break;
    case TextKind::Help: n::PrintHelp(k.c_str()); break;
    case TextKind::HelpForever: n::PrintHelpForever(k.c_str()); break;
    case TextKind::ClearPrints: n::ClearPrints(); break;
    case TextKind::ClearHelp: n::ClearHelp(); break;
    case TextKind::ClearThisPrint: n::ClearThisPrint(k.c_str()); break;
    case TextKind::StringInStringNow: n::PrintStringInStringNow(k.c_str(), k2.c_str(), t.time_ms, t.flag); break;
    case TextKind::WithNumber: n::PrintWithNumber(k.c_str(), t.number, t.time_ms, t.flag); break;
    case TextKind::WithNumberNow: n::PrintWithNumberNow(k.c_str(), t.number, t.time_ms, t.flag); break;
  }
}

} // namespace

void configure(const Settings& s) { g_cfg = s; }

bool is_story_script(const std::string& name) {
  if (name.empty()) return false;
  // Always local, whatever the ini says: the phone (every player needs theirs, e.g. for
  // the co-op menu while joined).
  static const char* kAlwaysLocal[] = {"spcellphone*"};
  for (const char* p : kAlwaysLocal)
    if (wildcard(p, name)) return false;
  for (const auto& p : g_cfg.local_scripts)
    if (wildcard(p, name)) return false;
  return true;
}

void register_hooks() {
  using native_hooks::on;
  using native_hooks::before;
  on(0x6A9F01AF, "REQUEST_ADDITIONAL_TEXT", [](const Call& c) { text_block(c, 0, 1); });
  on(0x0A1B465C, "CLEAR_ADDITIONAL_TEXT", [](const Call& c) { text_block(c, -1, 0); });
  on(0x537379A8, "ADD_SCORE", payout);
  world::register_hooks();
  dialogue::register_hooks();
  cutscene::register_hooks();
  // Text: (key, [key2 | number], time, flag) per native.
  on(0x73B01573, "PRINT_NOW", [](const Call& c) { text(c, TextKind::PrintNow, 0, -1, -1, 1, 2); });
  on(0x0A491CFF, "PRINT", [](const Call& c) { text(c, TextKind::Print, 0, -1, -1, 1, 2); });
  on(0x71076BBA, "PRINT_HELP", [](const Call& c) { text(c, TextKind::Help, 0, -1, -1, -1, -1); });
  on(0x43F7517D, "PRINT_HELP_FOREVER", [](const Call& c) { text(c, TextKind::HelpForever, 0, -1, -1, -1, -1); });
  on(0x1D8C324A, "CLEAR_PRINTS", [](const Call& c) { text(c, TextKind::ClearPrints, -1, -1, -1, -1, -1); });
  on(0x07244253, "CLEAR_HELP", [](const Call& c) { text(c, TextKind::ClearHelp, -1, -1, -1, -1, -1); });
  on(0x08D85CBB, "CLEAR_THIS_PRINT", [](const Call& c) { text(c, TextKind::ClearThisPrint, 0, -1, -1, -1, -1); });
  on(0x2BB65467, "PRINT_STRING_IN_STRING_NOW",
     [](const Call& c) { text(c, TextKind::StringInStringNow, 0, 1, -1, 2, 3); });
  on(0x76A63B4C, "PRINT_WITH_NUMBER", [](const Call& c) { text(c, TextKind::WithNumber, 0, -1, 1, 2, 3); });
  on(0x3BDA562E, "PRINT_WITH_NUMBER_NOW", [](const Call& c) { text(c, TextKind::WithNumberNow, 0, -1, 1, 2, 3); });

  // Blips.
  on(0x3E7D3074, "ADD_BLIP_FOR_COORD", [](const Call& c) { blip_coord(c, BlipKind::Coord); });
  on(0x7C671162, "ADD_BLIP_FOR_CONTACT", [](const Call& c) { blip_coord(c, BlipKind::Contact); });
  const auto entity_blip = [](const Call& c) {
    if (!capture(c)) return;
    const auto blip = static_cast<std::uint32_t>(c.out<std::int32_t>(1));
    if (!blip) return;
    BlipAdd add;
    add.id = blip, add.kind = BlipKind::Entity;
    g_host_blips[blip] = HostBlip{add, false, {}};
    g_ops.push_back(EntityBlipOp{blip, c.arg<n::Handle>(0)});
  };
  on(0x19A64C5D, "ADD_BLIP_FOR_CHAR", entity_blip);
  on(0x6D21564D, "ADD_BLIP_FOR_CAR", entity_blip);
  on(0x7BBF3625, "REMOVE_BLIP", [](const Call& c) {
    if (!capture(c)) return;
    const auto id = c.arg<std::uint32_t>(0);
    if (g_host_blips.erase(id)) send(encode_blip_remove(id));
  });
  on(0x1D8800E3, "CHANGE_BLIP_COLOUR", [](const Call& c) { blip_prop(c, BlipPropKind::Colour); });
  on(0x6A90123D, "CHANGE_BLIP_SPRITE", [](const Call& c) { blip_prop(c, BlipPropKind::Sprite); });
  on(0x3ACC1794, "CHANGE_BLIP_DISPLAY", [](const Call& c) { blip_prop(c, BlipPropKind::Display); });
  on(0x7B8D68E7, "SET_ROUTE", [](const Call& c) { blip_prop(c, BlipPropKind::Route); });
  on(0x44D349D9, "CHANGE_BLIP_SCALE", [](const Call& c) { blip_prop(c, BlipPropKind::Scale); });
  on(0x0A9D695E, "CHANGE_BLIP_NAME_FROM_TEXT_FILE", [](const Call& c) { blip_prop(c, BlipPropKind::Name); });

  // Mission entities: created -> networked (pinned to the host); deleted/released ->
  // removed from the network. (Objects come later.)
  on(0x4A673763, "CREATE_CHAR",
     [](const Call& c) { entity_created(c, sync::EntityType::Ped, c.arg<std::uint32_t>(1), 5); });
  on(0x2F1D6843, "CREATE_CAR",
     [](const Call& c) { entity_created(c, sync::EntityType::Vehicle, c.arg<std::uint32_t>(0), 4); });
  on(0x2702274D, "CREATE_CHAR_INSIDE_CAR",
     [](const Call& c) { entity_created(c, sync::EntityType::Ped, c.arg<std::uint32_t>(2), 3); });
  on(0x442B1C1D, "CREATE_CHAR_AS_PASSENGER",
     [](const Call& c) { entity_created(c, sync::EntityType::Ped, c.arg<std::uint32_t>(2), 4); });
  before(0x0E3B49BF, "DELETE_CHAR", entity_released);
  before(0x7F71342D, "DELETE_CAR", entity_released);
  before(0x0B774604, "MARK_CHAR_AS_NO_LONGER_NEEDED", entity_released);
  before(0x20C76FD1, "MARK_CAR_AS_NO_LONGER_NEEDED", entity_released);
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  world::tick();
  for (auto& msg : world::take_outbox())
    if (hosting) net.send_game(std::move(msg), true);
  dialogue::host_tick(hosting, net, ents);
  cutscene::host_tick(hosting, net);
  if (!hosting) {
    g_ops.clear();
    g_host_blips.clear();
    g_host_text_blocks.clear();
    return;
  }
  while (!g_ops.empty()) {
    Op op = std::move(g_ops.front());
    g_ops.pop_front();
    if (auto* s = std::get_if<SendOp>(&op)) {
      net.send_game(std::move(s->bytes), true);
      ++g_status.messages;
    } else if (auto* r = std::get_if<RegisterOp>(&op)) {
      if (ents.host_register(r->handle, r->type, r->model, net, /*pinned=*/true)) ++g_status.entities_registered;
    } else if (auto* u = std::get_if<UnregisterOp>(&op)) {
      ents.host_unregister_handle(u->handle, net);
    } else if (auto* b = std::get_if<EntityBlipOp>(&op)) {
      const auto hb = g_host_blips.find(b->blip);
      if (hb == g_host_blips.end()) continue; // removed before we got to it
      if (auto id = ents.id_for_handle(b->handle)) {
        hb->second.add.entity = *id;
        hb->second.resolved = true;
        net.send_game(encode(hb->second.add), true);
        ++g_status.messages;
      } else {
        log::warn("Story blip {} is on a non-networked entity {}; not replicated", b->blip, b->handle);
        g_host_blips.erase(hb); // its queued props go to an id clients don't know: ignored
      }
    }
  }
}

void host_on_player_joined(std::uint8_t player, net::Session& net) {
  // World state first (progression), then text blocks and blips.
  const auto ledger = world::encode_ledger();
  for (const auto& msg : ledger) net.send_game(msg, true);
  // Broadcast, like entity creates: clients that already have a blip/block keep it as is.
  // Called after the entity resend, so entity blips' entities arrive first (clients retry
  // a blip until its entity's copy exists anyway).
  for (const auto& [slot, name] : g_host_text_blocks) {
    TextBlock b;
    b.slot = slot, b.name = make_key(name);
    net.send_game(encode(b), true);
  }
  unsigned blips = 0;
  for (const auto& [id, hb] : g_host_blips) {
    if (!hb.resolved) continue; // host_tick sends it when it resolves
    net.send_game(encode(hb.add), true);
    for (const auto& [kind, p] : hb.props) net.send_game(encode(p), true);
    ++blips;
  }
  log::info("Story: sent {} world entries, {} text blocks and {} blips to late joiner #{}", ledger.size(),
            g_host_text_blocks.size(), blips, player);
}

bool client_on_message(const net::GameMessage& m, sync::EntitySync& ents) {
  if (!is_story_msg(m.data)) return false;
  if (m.from != net::kHostId) return true; // only the host tells the story
  const auto d = decode(m.data);
  if (!d) return true;
  ++g_status.messages;
  switch (d->type) {
    case StoryMsg::Text:
      if (g_loading_slots.empty()) apply_text(d->text);
      else g_held_texts.push_back(d->text);
      ++g_status.texts;
      break;
    case StoryMsg::World:
      world::client_apply(m.data);
      break;
    case StoryMsg::Conversation:
    case StoryMsg::ConversationAbort:
      dialogue::client_apply(m.data, ents);
      break;
    case StoryMsg::CutsceneStart:
    case StoryMsg::CutsceneEnd:
      cutscene::client_apply(m.data);
      break;
    case StoryMsg::Payout:
      n::AddScore(n::GetPlayerId(), d->payout);
      log::info("Story: mission payout ${} (mirrored from the host)", d->payout);
      break;
    case StoryMsg::TextBlock: {
      const std::uint8_t slot = d->text_block.slot;
      const std::string name = to_string(d->text_block.name);
      if (name.empty()) {
        n::ClearAdditionalText(slot, false);
        g_loading_slots.erase(slot), g_loaded_slots.erase(slot);
      } else if (auto it = g_loaded_slots.find(slot); it != g_loaded_slots.end() && it->second == name) {
        break; // already there (a late-join resend)
      } else {
        n::RequestAdditionalText(name.c_str(), slot);
        g_loading_slots.insert(slot), g_loaded_slots[slot] = name;
      }
      log::info("Story: text block slot {} -> {}", slot, name.empty() ? "(cleared)" : name);
      break;
    }
    case StoryMsg::BlipAdd: {
      auto& cb = g_client_blips[d->blip_add.id];
      const BlipAdd& a = d->blip_add;
      if (cb.local && same_place(cb.add, a)) {
        cb.props.clear(); // a late-join resend: keep the blip, its props follow again
        break;
      }
      if (cb.local) n::RemoveBlip(cb.local);
      cb = ClientBlip{0, a, {}};
      try_create(cb, ents);
      break;
    }
    case StoryMsg::BlipRemove:
      if (auto it = g_client_blips.find(d->blip_id); it != g_client_blips.end()) {
        if (it->second.local) n::RemoveBlip(it->second.local);
        g_client_blips.erase(it);
      }
      break;
    case StoryMsg::BlipProp:
      if (auto it = g_client_blips.find(d->blip_prop.id); it != g_client_blips.end()) {
        it->second.props.push_back(d->blip_prop);
        if (it->second.local) apply_prop(it->second.local, d->blip_prop);
      }
      break;
  }
  return true;
}

void client_tick(sync::EntitySync& ents) {
  std::erase_if(g_loading_slots, [](std::uint8_t slot) { return n::HasAdditionalTextLoaded(slot); });
  while (g_loading_slots.empty() && !g_held_texts.empty()) {
    apply_text(g_held_texts.front());
    g_held_texts.pop_front();
  }
  unsigned live = 0;
  for (auto& [id, cb] : g_client_blips) {
    if (!cb.local) try_create(cb, ents);
    if (cb.local) ++live;
  }
  g_status.blips_live = live;
}

std::vector<std::uint32_t> mission_cars(sync::EntitySync& ents) {
  // A car the host's mission hands the protagonist: blipped, non-hostile, empty. Colour is
  // the latest Colour prop (1 = red: enemies/targets); without one it isn't decided yet.
  std::vector<std::uint32_t> out;
  const DWORD now = GetTickCount();
  for (const auto& [id, cb] : g_client_blips) {
    if (cb.add.kind != BlipKind::Entity || now - cb.since < 1000) continue;
    std::optional<std::int32_t> colour;
    for (const auto& p : cb.props)
      if (p.prop == BlipPropKind::Colour) colour = p.ivalue;
    if (!colour || *colour == 1) continue;
    const n::Handle h = ents.handle_for_id(cb.add.entity);
    if (!h || !n::DoesVehicleExist(h) || n::GetDriverOfCar(h)) continue;
    out.push_back(cb.add.entity);
  }
  return out;
}

void clear() {
  world::client_restore();
  dialogue::clear();
  cutscene::clear();
  for (auto& [id, cb] : g_client_blips)
    if (cb.local) n::RemoveBlip(cb.local);
  g_client_blips.clear();
  // Free what the host's story loaded. Blocks our own (frozen) scripts had in those slots
  // were replaced when we loaded the host's; restoring them belongs with progression sync.
  for (const auto& [slot, name] : g_loaded_slots) n::ClearAdditionalText(slot, false);
  g_loading_slots.clear(), g_loaded_slots.clear(), g_held_texts.clear();
  g_host_blips.clear();
  g_host_text_blocks.clear();
  g_ops.clear();
  g_hosting = false;
}

Status status() { return g_status; }

} // namespace ivc::story
