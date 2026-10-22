#include "story/dialogue.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/native_hooks.h"
#include "net/bytes.h"
#include "story/story_protocol.h"
#include "story/story_sync.h"

namespace ivc::story::dialogue {
namespace {
namespace n = natives;
using n::invoke;
using native_hooks::Call;

constexpr std::uint32_t kNewConversation = 0x6C213305, kAddSpeaker = 0x542D499E, kAddLine = 0x75080482,
                        kStartConversation = 0x288E50A3, kNewPhoneCall = 0x720E7EA6, kAddPhoneLine = 0x0BED1DDE,
                        kStartPhoneCall = 0x7939764F, kAbort = 0x57DB70CE;
constexpr int kSpeakers = 10;

enum class SpeakerKind : std::uint8_t { None, HostPlayer, Entity, Unknown };

struct Speaker {
  SpeakerKind kind = SpeakerKind::None;
  n::Handle ped = 0;        // host: the script's ped handle (resolved when sending)
  std::uint32_t entity = 0; // networked entity id
  Key voice{};
};

struct Line {
  std::int8_t speaker = 0, listener = 0;
  std::int32_t flag = 0;
  Key context{}, subtitle{};
};

struct Conversation {
  bool phone = false;
  std::int32_t start_a = 0, start_b = 0;
  std::array<Speaker, kSpeakers> speakers{};
  std::vector<Line> lines;
};

// ---------------------------------------------------------------- host capture

bool g_hosting = false;
std::optional<Conversation> g_building;
std::deque<Conversation> g_ready; // started; sent by host_tick (speakers resolved there)
bool g_abort = false;

// Who builds dialogue: speechcontrol, the phone's calling script, and mission scripts.
bool dialogue_script(const Call& c) {
  return g_hosting && (c.script == "speechcontrol" || c.script == "spcellphonecalling" || is_story_script(c.script));
}

std::vector<std::uint8_t> encode(const Conversation& conv) {
  net::Writer w;
  w.put(StoryMsg::Conversation).put(std::uint8_t{0});
  w.put(static_cast<std::uint8_t>(conv.phone)).put(conv.start_a).put(conv.start_b);
  for (const Speaker& s : conv.speakers) w.put(s.kind).put(s.entity).put(s.voice);
  w.put(static_cast<std::uint8_t>(conv.lines.size()));
  for (const Line& l : conv.lines) w.put(l.speaker).put(l.listener).put(l.flag).put(l.context).put(l.subtitle);
  return std::move(w.data());
}

// ---------------------------------------------------------------- client playback

std::function<n::Ped()> g_host_ped;

n::Ped local_ped(const Speaker& s, sync::EntitySync& ents) {
  if (s.kind == SpeakerKind::Entity)
    if (const n::Handle h = ents.handle_for_id(s.entity); h && n::DoesCharExist(h)) return h;
  const n::Ped host = g_host_ped ? g_host_ped() : 0; // the host's puppet; also the fallback
  return host && n::DoesCharExist(host) ? host : 0;
}

void play(const Conversation& conv, sync::EntitySync& ents) {
  // Build it as a scripted conversation even for phone calls: that plays positionally from
  // the speaker peds (a phone call plays as the local player's handset audio).
  invoke(kNewConversation);
  for (const Line& l : conv.lines) {
    const std::string ctx = to_string(l.context), sub = to_string(l.subtitle);
    invoke(kAddLine, static_cast<std::int32_t>(l.speaker), ctx.c_str(), sub.c_str(),
           static_cast<std::int32_t>(l.listener), l.flag);
  }
  int speakers = 0;
  for (int slot = 0; slot < kSpeakers; ++slot) {
    const Speaker& s = conv.speakers[slot];
    if (s.kind == SpeakerKind::None) continue;
    const n::Ped ped = local_ped(s, ents);
    if (!ped) continue;
    const std::string voice = to_string(s.voice);
    invoke(kAddSpeaker, slot, ped, voice.c_str());
    ++speakers;
  }
  if (!speakers) {
    log::info("Dialogue: no local speaker for a {} ({} lines); skipped", conv.phone ? "phone call" : "conversation",
              conv.lines.size());
    return;
  }
  invoke(kStartConversation, conv.phone ? 1 : conv.start_a, conv.phone ? 0 : conv.start_b);
  log::info("Dialogue: playing a {} ({} lines, {} speakers)", conv.phone ? "phone call" : "conversation",
            conv.lines.size(), speakers);
}

} // namespace

void register_hooks() {
  using native_hooks::on;
  on(kNewConversation, "NEW_SCRIPTED_CONVERSATION", [](const Call& c) {
    if (dialogue_script(c)) g_building = Conversation{};
  });
  on(kNewPhoneCall, "NEW_MOBILE_PHONE_CALL", [](const Call& c) {
    if (!dialogue_script(c)) return;
    g_building = Conversation{};
    g_building->phone = true;
  });
  on(kAddLine, "ADD_LINE_TO_CONVERSATION", [](const Call& c) {
    if (!dialogue_script(c) || !g_building) return;
    Line l;
    l.speaker = static_cast<std::int8_t>(c.arg<std::int32_t>(0));
    l.context = make_key(c.str(1));
    l.subtitle = make_key(c.str(2));
    l.listener = static_cast<std::int8_t>(c.arg<std::int32_t>(3));
    l.flag = c.arg<std::int32_t>(4);
    g_building->lines.push_back(l);
  });
  on(kAddPhoneLine, "ADD_LINE_TO_MOBILE_PHONE_CALL", [](const Call& c) {
    if (!dialogue_script(c) || !g_building) return;
    Line l;
    l.speaker = static_cast<std::int8_t>(c.arg<std::int32_t>(0));
    l.listener = l.speaker;
    l.context = make_key(c.str(1));
    l.subtitle = make_key(c.str(2));
    g_building->lines.push_back(l);
  });
  on(kAddSpeaker, "ADD_NEW_CONVERSATION_SPEAKER", [](const Call& c) {
    if (!dialogue_script(c) || !g_building) return;
    const int slot = c.arg<std::int32_t>(0);
    if (slot < 0 || slot >= kSpeakers) return;
    Speaker& s = g_building->speakers[slot];
    s.kind = SpeakerKind::Unknown;
    s.ped = c.arg<n::Handle>(1);
    s.voice = make_key(c.str(2));
  });
  on(kStartConversation, "START_SCRIPT_CONVERSATION", [](const Call& c) {
    if (!dialogue_script(c) || !g_building || g_building->phone) return;
    g_building->start_a = c.arg<std::int32_t>(0), g_building->start_b = c.arg<std::int32_t>(1);
    g_ready.push_back(std::move(*g_building));
    g_building.reset();
  });
  on(kStartPhoneCall, "START_MOBILE_PHONE_CALL", [](const Call& c) {
    if (!dialogue_script(c) || !g_building || !g_building->phone) return;
    // Speaker 0 is the caller-side ped/voice (the player), 1 the other end.
    for (int i = 0; i < 2; ++i) {
      Speaker& s = g_building->speakers[i];
      s.kind = SpeakerKind::Unknown;
      s.ped = c.arg<n::Handle>(i * 2);
      s.voice = make_key(c.str(i * 2 + 1));
    }
    g_ready.push_back(std::move(*g_building));
    g_building.reset();
  });
  on(kAbort, "ABORT_SCRIPTED_CONVERSATION", [](const Call& c) {
    if (dialogue_script(c)) g_abort = true;
  });
}

void host_tick(bool hosting, net::Session& net, sync::EntitySync& ents) {
  g_hosting = hosting;
  if (!hosting) {
    g_building.reset();
    g_ready.clear();
    g_abort = false;
    return;
  }
  const n::Ped me = n::GetPlayerChar(n::GetPlayerId());
  while (!g_ready.empty()) {
    Conversation conv = std::move(g_ready.front());
    g_ready.pop_front();
    for (Speaker& s : conv.speakers) {
      if (s.kind == SpeakerKind::None) continue;
      if (s.ped == me || !s.ped) {
        s.kind = SpeakerKind::HostPlayer; // no ped (e.g. the far end of a call) plays from the host too
      } else if (const auto id = ents.id_for_handle(s.ped)) {
        s.kind = SpeakerKind::Entity, s.entity = *id;
      } else {
        s.kind = SpeakerKind::HostPlayer; // not networked: nearest stand-in is the host
      }
    }
    net.send_game(encode(conv), true);
    log::info("Dialogue: sent a {} ({} lines)", conv.phone ? "phone call" : "conversation", conv.lines.size());
  }
  if (g_abort) {
    g_abort = false;
    net::Writer w;
    w.put(StoryMsg::ConversationAbort).put(std::uint8_t{0});
    net.send_game(std::move(w.data()), true);
  }
}

void client_apply(std::span<const std::uint8_t> msg, sync::EntitySync& ents) {
  net::Reader r(msg);
  StoryMsg type;
  std::uint8_t sender;
  if (!r.get(type) || !r.get(sender)) return;
  if (type == StoryMsg::ConversationAbort) {
    invoke(kAbort, false);
    return;
  }
  Conversation conv;
  std::uint8_t phone = 0, lines = 0;
  r.get(phone), r.get(conv.start_a), r.get(conv.start_b);
  conv.phone = phone != 0;
  for (Speaker& s : conv.speakers) r.get(s.kind), r.get(s.entity), r.get(s.voice);
  r.get(lines);
  for (int i = 0; i < lines && r.ok(); ++i) {
    Line l;
    r.get(l.speaker), r.get(l.listener), r.get(l.flag), r.get(l.context), r.get(l.subtitle);
    conv.lines.push_back(l);
  }
  if (!r.ok()) return;
  play(conv, ents);
}

void set_host_ped_provider(std::function<n::Ped()> provider) { g_host_ped = std::move(provider); }

void clear() {
  g_building.reset();
  g_ready.clear();
  g_abort = false;
}

} // namespace ivc::story::dialogue
