#pragma once
// Story replication messages (M6), host -> all, reliable, on the game-message lane.
// Text is sent as game-text (GXT) keys, so every client shows the identical localised
// line; blips refer to networked entities by entity id.

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/bytes.h"
#include "net/protocol.h"

namespace ivc::story {

enum class StoryMsg : std::uint8_t {
  Text = 21,
  BlipAdd = 22,
  BlipRemove = 23,
  BlipProp = 24,
  TextBlock = 25,
  World = 26,  // a world-state ledger op (world_state.cpp owns the payload)
  Payout = 27, // a mission paid the protagonist: int32 amount
  Conversation = 28,      // dialogue.cpp owns the payload
  ConversationAbort = 29,
  CutsceneStart = 30, // Key name (cutscene.cpp)
  CutsceneEnd = 31,
};

using Key = std::array<char, 32>;
inline Key make_key(const std::string& s) {
  Key k{};
  std::memcpy(k.data(), s.data(), std::min(s.size(), k.size() - 1));
  return k;
}
inline std::string to_string(const Key& k) { return std::string(k.data(), strnlen(k.data(), k.size())); }

enum class TextKind : std::uint8_t {
  Print, PrintNow, Help, HelpForever, ClearPrints, ClearHelp, ClearThisPrint, StringInStringNow, WithNumber,
  WithNumberNow,
};

struct Text {
  TextKind kind{};
  Key key{}, key2{};
  std::int32_t number = 0;
  std::uint32_t time_ms = 0;
  std::uint8_t flag = 0;
};

enum class BlipKind : std::uint8_t { Coord, Contact, Entity };
struct BlipAdd {
  std::uint32_t id = 0; // host's blip handle (stable for the blip's life)
  BlipKind kind{};
  float x = 0, y = 0, z = 0;
  std::uint32_t entity = 0; // networked entity id (BlipKind::Entity)
};

enum class BlipPropKind : std::uint8_t { Colour, Sprite, Display, Route, Scale, Name };
struct BlipProp {
  std::uint32_t id = 0;
  BlipPropKind prop{};
  std::int32_t ivalue = 0;
  float fvalue = 0;
  Key name{};
};

// Mission lines live in per-mission text blocks the script loads into a slot first
// (REQUEST_ADDITIONAL_TEXT); a joiner's frozen scripts never do, so the host sends it.
// An empty name clears the slot.
struct TextBlock {
  std::uint8_t slot = 0;
  Key name{};
};

inline std::vector<std::uint8_t> encode(const TextBlock& b) {
  net::Writer w;
  w.put(StoryMsg::TextBlock).put(std::uint8_t{0}).put(b.slot).put(b.name);
  return std::move(w.data());
}
inline std::vector<std::uint8_t> encode_payout(std::int32_t amount) {
  net::Writer w;
  w.put(StoryMsg::Payout).put(std::uint8_t{0}).put(amount);
  return std::move(w.data());
}
inline std::vector<std::uint8_t> encode(const Text& t) {
  net::Writer w;
  w.put(StoryMsg::Text).put(std::uint8_t{0}).put(t.kind).put(t.key).put(t.key2).put(t.number).put(t.time_ms).put(t.flag);
  return std::move(w.data());
}
inline std::vector<std::uint8_t> encode(const BlipAdd& b) {
  net::Writer w;
  w.put(StoryMsg::BlipAdd).put(std::uint8_t{0}).put(b.id).put(b.kind).put(b.x).put(b.y).put(b.z).put(b.entity);
  return std::move(w.data());
}
inline std::vector<std::uint8_t> encode_blip_remove(std::uint32_t id) {
  net::Writer w;
  w.put(StoryMsg::BlipRemove).put(std::uint8_t{0}).put(id);
  return std::move(w.data());
}
inline std::vector<std::uint8_t> encode(const BlipProp& p) {
  net::Writer w;
  w.put(StoryMsg::BlipProp).put(std::uint8_t{0}).put(p.id).put(p.prop).put(p.ivalue).put(p.fvalue).put(p.name);
  return std::move(w.data());
}

struct Decoded {
  StoryMsg type{};
  Text text;
  BlipAdd blip_add;
  std::uint32_t blip_id = 0; // BlipRemove
  BlipProp blip_prop;
  TextBlock text_block;
  std::int32_t payout = 0;
};

inline bool is_story_msg(std::span<const std::uint8_t> d) {
  return !d.empty() && d[0] >= static_cast<std::uint8_t>(StoryMsg::Text) &&
         d[0] <= static_cast<std::uint8_t>(StoryMsg::CutsceneEnd);
}

inline std::optional<Decoded> decode(std::span<const std::uint8_t> d) {
  net::Reader r(d);
  Decoded m;
  std::uint8_t sender;
  r.get(m.type), r.get(sender);
  switch (m.type) {
    case StoryMsg::Text: {
      auto& t = m.text;
      r.get(t.kind), r.get(t.key), r.get(t.key2), r.get(t.number), r.get(t.time_ms), r.get(t.flag);
      break;
    }
    case StoryMsg::BlipAdd: {
      auto& b = m.blip_add;
      r.get(b.id), r.get(b.kind), r.get(b.x), r.get(b.y), r.get(b.z), r.get(b.entity);
      break;
    }
    case StoryMsg::BlipRemove:
      r.get(m.blip_id);
      break;
    case StoryMsg::TextBlock:
      r.get(m.text_block.slot), r.get(m.text_block.name);
      break;
    case StoryMsg::Payout:
      r.get(m.payout);
      break;
    case StoryMsg::World:
    case StoryMsg::Conversation:
    case StoryMsg::ConversationAbort:
    case StoryMsg::CutsceneStart:
    case StoryMsg::CutsceneEnd:
      break; // payloads decoded by world_state / dialogue / cutscene
    case StoryMsg::BlipProp: {
      auto& p = m.blip_prop;
      r.get(p.id), r.get(p.prop), r.get(p.ivalue), r.get(p.fvalue), r.get(p.name);
      break;
    }
    default:
      return std::nullopt;
  }
  if (!r.ok()) return std::nullopt;
  return m;
}

} // namespace ivc::story
