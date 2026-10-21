#include "story/call_log.h"

#include <format>
#include <string>

#include "core/log.h"
#include "game/native_hooks.h"

namespace ivc::story {
namespace {

using native_hooks::Call;

std::string str(const Call& c, int i) { return c.str(i); } // pointer-validated

void report(const Call& c, const std::string& detail) { log::info("[story] {:<14} {}({})", c.script, c.name, detail); }

} // namespace

void register_call_logging() {
  using H = native_hooks::Callback;
  const auto on = [](std::uint32_t hash, const char* name, H cb) { native_hooks::on(hash, name, std::move(cb)); };

  // --- on-screen text (args: GXT key first) ---
  for (auto [hash, name] : {std::pair{0x73B01573u, "PRINT_NOW"}, {0x0A491CFFu, "PRINT"},
                            {0x71076BBAu, "PRINT_HELP"}, {0x43F7517Du, "PRINT_HELP_FOREVER"},
                            {0x08D85CBBu, "CLEAR_THIS_PRINT"}})
    on(hash, name, [](const Call& c) { report(c, str(c, 0)); });
  on(0x2BB65467, "PRINT_STRING_IN_STRING_NOW",
     [](const Call& c) { report(c, std::format("{}, {}", str(c, 0), str(c, 1))); });
  for (auto [hash, name] : {std::pair{0x3BDA562Eu, "PRINT_WITH_NUMBER_NOW"}, {0x76A63B4Cu, "PRINT_WITH_NUMBER"}})
    on(hash, name, [](const Call& c) { report(c, std::format("{}, {}", str(c, 0), c.arg<int>(1))); });
  for (auto [hash, name] : {std::pair{0x1D8C324Au, "CLEAR_PRINTS"}, {0x07244253u, "CLEAR_HELP"}})
    on(hash, name, [](const Call& c) { report(c, ""); });

  // --- blips ---
  for (auto [hash, name] : {std::pair{0x3E7D3074u, "ADD_BLIP_FOR_COORD"}, {0x7C671162u, "ADD_BLIP_FOR_CONTACT"}})
    on(hash, name, [](const Call& c) {
      report(c, std::format("{:.1f}, {:.1f}, {:.1f} -> blip {}", c.arg<float>(0), c.arg<float>(1), c.arg<float>(2),
                            c.out<int>(3)));
    });
  for (auto [hash, name] : {std::pair{0x19A64C5Du, "ADD_BLIP_FOR_CHAR"}, {0x6D21564Du, "ADD_BLIP_FOR_CAR"},
                            {0x70CC1487u, "ADD_BLIP_FOR_OBJECT"}})
    on(hash, name, [](const Call& c) { report(c, std::format("entity {} -> blip {}", c.arg<int>(0), c.out<int>(1))); });
  on(0x7BBF3625, "REMOVE_BLIP", [](const Call& c) { report(c, std::format("blip {}", c.arg<int>(0))); });
  for (auto [hash, name] : {std::pair{0x1D8800E3u, "CHANGE_BLIP_COLOUR"}, {0x6A90123Du, "CHANGE_BLIP_SPRITE"},
                            {0x3ACC1794u, "CHANGE_BLIP_DISPLAY"}, {0x7B8D68E7u, "SET_ROUTE"}})
    on(hash, name, [](const Call& c) { report(c, std::format("blip {}, {}", c.arg<int>(0), c.arg<int>(1))); });
  on(0x44D349D9, "CHANGE_BLIP_SCALE",
     [](const Call& c) { report(c, std::format("blip {}, {:.2f}", c.arg<int>(0), c.arg<float>(1))); });
  on(0x0A9D695E, "CHANGE_BLIP_NAME_FROM_TEXT_FILE",
     [](const Call& c) { report(c, std::format("blip {}, {}", c.arg<int>(0), str(c, 1))); });

  // --- entities ---
  on(0x4A673763, "CREATE_CHAR", [](const Call& c) {
    report(c, std::format("type {}, model 0x{:08X}, {:.1f} {:.1f} {:.1f} -> ped {}", c.arg<int>(0),
                          c.arg<unsigned>(1), c.arg<float>(2), c.arg<float>(3), c.arg<float>(4), c.out<int>(5)));
  });
  on(0x2F1D6843, "CREATE_CAR", [](const Call& c) {
    report(c, std::format("model 0x{:08X}, {:.1f} {:.1f} {:.1f} -> car {}", c.arg<unsigned>(0), c.arg<float>(1),
                          c.arg<float>(2), c.arg<float>(3), c.out<int>(4)));
  });
  on(0x4DE152A0, "CREATE_OBJECT", [](const Call& c) {
    report(c, std::format("model 0x{:08X}, {:.1f} {:.1f} {:.1f} -> object {}", c.arg<unsigned>(0), c.arg<float>(1),
                          c.arg<float>(2), c.arg<float>(3), c.out<int>(4)));
  });
  on(0x2702274D, "CREATE_CHAR_INSIDE_CAR", [](const Call& c) {
    report(c, std::format("car {}, model 0x{:08X} -> ped {}", c.arg<int>(0), c.arg<unsigned>(2), c.out<int>(3)));
  });
  on(0x442B1C1D, "CREATE_CHAR_AS_PASSENGER", [](const Call& c) {
    report(c, std::format("car {}, model 0x{:08X}, seat {} -> ped {}", c.arg<int>(0), c.arg<unsigned>(2),
                          c.arg<int>(3), c.out<int>(4)));
  });
  // DELETE_* / MARK_*_NO_LONGER_NEEDED take a pointer that the native zeroes, so the
  // value is gone after the call; report that it happened.
  for (auto [hash, name] : {std::pair{0x0E3B49BFu, "DELETE_CHAR"}, {0x7F71342Du, "DELETE_CAR"},
                            {0x0B774604u, "MARK_CHAR_AS_NO_LONGER_NEEDED"}, {0x20C76FD1u, "MARK_CAR_AS_NO_LONGER_NEEDED"}})
    on(hash, name, [](const Call& c) { report(c, ""); });

  // --- flow ---
  on(0x4E2260B9, "START_NEW_SCRIPT", [](const Call& c) {
    report(c, std::format("{}, stack {} -> thread {}", str(c, 0), c.arg<int>(1), c.result<int>()));
  });
  on(0x53591DD7, "START_CUTSCENE_NOW", [](const Call& c) { report(c, str(c, 0)); });
  on(0x60C80EC9, "ALTER_WANTED_LEVEL",
     [](const Call& c) { report(c, std::format("player {}, level {}", c.arg<int>(0), c.arg<int>(1))); });
  on(0x6C213305, "NEW_SCRIPTED_CONVERSATION", [](const Call& c) { report(c, ""); });
  // Types partly undocumented; args 1-2 look like text pointers (validated reads are safe).
  on(0x75080482, "ADD_LINE_TO_CONVERSATION", [](const Call& c) {
    report(c, std::format("{}, {}, {}, {}, {}", c.arg<int>(0), str(c, 1), str(c, 2), c.arg<int>(3), c.arg<int>(4)));
  });
  on(0x288E50A3, "START_SCRIPT_CONVERSATION",
     [](const Call& c) { report(c, std::format("{}, {}", c.arg<int>(0), c.arg<int>(1))); });

  // --- world state (progression research): what story scripts change persistently ---
  const auto f6 = [](const Call& c, int at) {
    return std::format("{:.1f} {:.1f} {:.1f} .. {:.1f} {:.1f} {:.1f}", c.arg<float>(at), c.arg<float>(at + 1),
                       c.arg<float>(at + 2), c.arg<float>(at + 3), c.arg<float>(at + 4), c.arg<float>(at + 5));
  };
  for (auto [hash, name] : {std::pair{0x59FD4E83u, "REQUEST_IPL"}, {0x787F38B5u, "REMOVE_IPL"},
                            {0x658F21AFu, "REMOVE_IPL_DISCREETLY"}})
    on(hash, name, [](const Call& c) { report(c, str(c, 0)); });
  for (auto [hash, name] : {std::pair{0x56553F38u, "SWITCH_ROADS_ON"}, {0x4C3C1F3Cu, "SWITCH_ROADS_OFF"},
                            {0x6251618Fu, "SWITCH_ROADS_BACK_TO_ORIGINAL"},
                            {0x6AA20B7Eu, "SWITCH_PED_ROADS_BACK_TO_ORIGINAL"}})
    on(hash, name, [f6](const Call& c) { report(c, f6(c, 0)); });
  on(0x69CE154F, "SET_CAR_GENERATORS_ACTIVE_IN_AREA",
     [f6](const Call& c) { report(c, std::format("{}, {}", f6(c, 0), c.arg<int>(6))); });
  on(0x5E077484, "SWAP_NEAREST_BUILDING_MODEL", [](const Call& c) {
    report(c, std::format("{:.1f} {:.1f} {:.1f} r{:.1f}, 0x{:08X} -> 0x{:08X}", c.arg<float>(0), c.arg<float>(1),
                          c.arg<float>(2), c.arg<float>(3), c.arg<unsigned>(4), c.arg<unsigned>(5)));
  });
  on(0x10974B70, "SET_STATE_OF_CLOSEST_DOOR_OF_TYPE", [](const Call& c) {
    report(c, std::format("0x{:08X}, {:.1f} {:.1f} {:.1f}, state {}, {:.2f}", c.arg<unsigned>(0), c.arg<float>(1),
                          c.arg<float>(2), c.arg<float>(3), c.arg<int>(4), c.arg<float>(5)));
  });
  on(0x0F132F7E, "CREATE_CAR_GENERATOR", [](const Call& c) {
    report(c, std::format("{:.1f} {:.1f} {:.1f}, model 0x{:08X} -> gen {}", c.arg<float>(0), c.arg<float>(1),
                          c.arg<float>(2), c.arg<unsigned>(6), c.out<int>(14)));
  });
  for (auto [hash, name] : {std::pair{0x4B6E368Du, "SET_EXTRA_HOSPITAL_RESTART_POINT"},
                            {0x1C4E7A79u, "SET_EXTRA_POLICE_STATION_RESTART_POINT"}})
    on(hash, name, [](const Call& c) {
      report(c, std::format("{:.1f} {:.1f} {:.1f}, {:.1f}, {:.1f}", c.arg<float>(0), c.arg<float>(1), c.arg<float>(2),
                            c.arg<float>(3), c.arg<float>(4)));
    });
  for (auto [hash, name] : {std::pair{0x70582D53u, "SET_ZONE_POPULATION_TYPE"}, {0x5E5E4252u, "SET_ZONE_SCUMMINESS"},
                            {0x64F37F05u, "SET_ZONE_NO_COPS"}})
    on(hash, name, [](const Call& c) { report(c, std::format("{}, {}", str(c, 0), c.arg<int>(1))); });
  for (auto [hash, name] : {std::pair{0x7CE83A30u, "SWITCH_CAR_GENERATOR"}, {0x66DD4F67u, "ACTIVATE_INTERIOR"},
                            {0x208C03C9u, "ENABLE_SAVE_HOUSE"}, {0x581E2306u, "DISABLE_CAR_GENERATORS"},
                            {0x537379A8u, "ADD_SCORE"}})
    on(hash, name, [](const Call& c) { report(c, std::format("{}, {}", c.arg<int>(0), c.arg<int>(1))); });
  for (auto [hash, name] : {std::pair{0x5D622498u, "SET_MAX_WANTED_LEVEL"}, {0x69A72C50u, "ALLOW_EMERGENCY_SERVICES"},
                            {0x0FFD1A92u, "SWITCH_RANDOM_TRAINS"}, {0x7FC65855u, "SWITCH_RANDOM_BOATS"},
                            {0x4E637988u, "SWITCH_AMBIENT_PLANES"}, {0x060669FEu, "SWITCH_GARBAGE_TRUCKS"},
                            {0x3A8531E8u, "SET_GANG_CAR"}})
    on(hash, name, [](const Call& c) { report(c, std::to_string(c.arg<int>(0))); });
}

} // namespace ivc::story
