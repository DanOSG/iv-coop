#include "story/coverage.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "game/native_hooks.h"
#include "game/natives.h"
#include "game/phone.h"
#include "game/script.h"

namespace ivc::story::coverage {
namespace {
namespace n = natives;
using n::invoke;
using native_hooks::Call;

struct Native {
  std::uint32_t hash;
  const char* name;
  const char* category;
};
constexpr Native kNatives[] = {
#include "story/coverage_natives.inc"
};

constexpr std::uint32_t kDoesScriptExist = 0x1D1B266B, kRequestScript = 0x6FFE0DFD, kHasScriptLoaded = 0x2A171915,
                        kStartNewScript = 0x4E2260B9, kMarkScriptNoLongerNeeded = 0x09E405DB,
                        kTerminateScripts = 0x72452672, kFadeIn = 0x04D72200, kSetPlayerControl = 0x1A6203EA;
constexpr std::uint32_t kFlushEveryMs = 30000, kLoadTimeoutMs = 15000;
constexpr std::size_t kMaxSamples = 6;

struct Stat {
  std::string category;
  std::uint64_t count = 0;
  std::set<std::string> samples;
};
using Key = std::pair<std::string, std::string>; // script, native

std::filesystem::path g_dir, g_plugin_dir;
unsigned g_stack = 8192;
std::map<Key, Stat> g_stats;
bool g_dirty = false;
std::uint32_t g_last_flush_ms = 0;

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Text arguments worth keeping as examples (anim/sound/effect names): only real, short,
// printable strings, since a number argument can happen to point at readable memory.
void add_samples(const Call& c, Stat& st) {
  if (st.samples.size() >= kMaxSamples) return;
  std::string joined;
  for (int i = 0; i < std::min(c.arg_count, 6); ++i) {
    if (c.arg<std::uint32_t>(i) < 0x10000) continue;
    const auto p = c.arg<const char*>(i);
    if (!native_hooks::readable_cstr(p, 64)) continue;
    const std::string s = p;
    if (s.size() < 2 || s.size() > 48 ||
        !std::ranges::all_of(s, [](char ch) { return ch >= 0x20 && ch < 0x7F && ch != '\t' && ch != ';'; }))
      continue;
    joined += (joined.empty() ? "" : " ") + s;
  }
  if (!joined.empty()) st.samples.insert(joined);
}

void load() {
  std::ifstream in(g_dir / "coverage.tsv");
  std::string line;
  std::getline(in, line); // header
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    for (std::string part; std::getline(ss, part, '\t');) f.push_back(part);
    if (f.size() < 4) continue;
    Stat& st = g_stats[{f[0], f[1]}];
    st.category = f[2];
    st.count += std::strtoull(f[3].c_str(), nullptr, 10);
    if (f.size() > 4) {
      std::stringstream sm(f[4]);
      for (std::string s; std::getline(sm, s, ';');)
        if (s.size() > 1) st.samples.insert(s.substr(s.front() == ' ' ? 1 : 0));
    }
  }
}

void flush() {
  g_dirty = false;
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  {
    std::ofstream out(g_dir / "coverage.tsv");
    out << "script\tnative\tcategory\tcalls\tsamples\n";
    for (const auto& [k, st] : g_stats) {
      out << k.first << '\t' << k.second << '\t' << st.category << '\t' << st.count << '\t';
      bool first = true;
      for (const auto& s : st.samples) out << (first ? "" : "; ") << s, first = false;
      out << '\n';
    }
  }
  // Summary: per category, each native's total calls and which scripts made them.
  struct Row {
    std::uint64_t calls = 0;
    std::map<std::string, std::uint64_t> scripts;
    std::set<std::string> samples;
  };
  std::map<std::string, std::map<std::string, Row>> cats;
  for (const auto& [k, st] : g_stats) {
    Row& r = cats[st.category][k.second];
    r.calls += st.count;
    r.scripts[k.first] += st.count;
    for (const auto& s : st.samples)
      if (r.samples.size() < kMaxSamples) r.samples.insert(s);
  }
  std::ofstream md(g_dir / "coverage.md");
  md << "# Sync coverage (played)\n\nCalls story scripts made to natives co-op doesn't sync yet, counted while "
        "playing (all sessions so far; delete coverage.tsv to start over).\n";
  for (const auto& [cat, natives] : cats) {
    std::uint64_t total = 0;
    for (const auto& [_, r] : natives) total += r.calls;
    md << std::format("\n## {} ({} calls)\n\n| Native | Calls | Scripts | Examples |\n|---|---|---|---|\n", cat, total);
    std::vector<std::pair<std::string, const Row*>> rows;
    for (const auto& [name, r] : natives) rows.emplace_back(name, &r);
    std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second->calls > b.second->calls; });
    for (const auto& [name, r] : rows) {
      std::string scripts, samples;
      for (const auto& [s, c] : r->scripts) scripts += std::format("{}{} {}", scripts.empty() ? "" : ", ", s, c);
      for (const auto& s : r->samples) samples += (samples.empty() ? "" : "; ") + s;
      md << "| " << name << " | " << r->calls << " | " << scripts << " | " << samples << " |\n";
    }
  }
}

void on_call(const Call& c, const char* category) {
  Stat& st = g_stats[{lower(c.script), c.name}];
  if (st.category.empty()) st.category = category;
  ++st.count;
  add_samples(c, st);
  g_dirty = true;
}

// --- mission launcher ---

bool g_down = true;
std::string g_running;  // script we started (lowercase), while it runs
std::string g_loading;  // requested, not loaded yet
std::uint32_t g_loading_ms = 0, g_started_ms = 0;

std::vector<std::string> read_lines(const std::filesystem::path& p) {
  std::vector<std::string> out;
  std::ifstream in(p);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    if (!line.empty() && line[0] != '#') out.push_back(lower(line));
  }
  return out;
}

bool running(const std::string& name) {
  for (const auto& t : script::threads())
    if (lower(t.name) == name) return true;
  return false;
}

void mark_done(const std::string& name) {
  std::ofstream(g_plugin_dir / "playlist_done.txt", std::ios::app) << name << '\n';
}

void say(const std::string& text) {
  log::warn("Launcher: {}", text);
  phone::notify(text);
}

void launch_next() {
  const auto list = read_lines(g_plugin_dir / "playlist.txt");
  const auto done_list = read_lines(g_plugin_dir / "playlist_done.txt");
  const std::set<std::string> done(done_list.begin(), done_list.end());
  std::size_t index = 0;
  for (const auto& name : list) {
    ++index;
    if (done.contains(name)) continue;
    if (!invoke<bool>(kDoesScriptExist, name.c_str())) {
      log::warn("Launcher: no script named {}; skipped", name);
      mark_done(name);
      continue;
    }
    invoke(kRequestScript, name.c_str());
    g_loading = name, g_loading_ms = GetTickCount();
    say(std::format("Starting {} ({} of {})", name, index, list.size()));
    return;
  }
  say(list.empty() ? "No playlist.txt" : "Playlist finished (clear playlist_done.txt to start over)");
}

void launcher_tick() {
  const std::uint32_t now = GetTickCount();
  if (input::pressed_ctrl_shift('M', g_down)) {
    if (!g_running.empty()) {
      invoke(kTerminateScripts, g_running.c_str());
      invoke(kFadeIn, 0u);
      invoke(kSetPlayerControl, n::GetPlayerId(), true);
      say(std::format("Ended {}; Ctrl+Shift+M starts the next", g_running));
      g_running.clear();
      return;
    }
    if (g_loading.empty()) launch_next();
  }
  if (!g_loading.empty()) {
    if (invoke<bool>(kHasScriptLoaded, g_loading.c_str())) {
      const auto id = invoke<std::uint32_t>(kStartNewScript, g_loading.c_str(), g_stack);
      invoke(kMarkScriptNoLongerNeeded, g_loading.c_str());
      log::info("Launcher: started {} (thread {})", g_loading, id);
      mark_done(g_loading);
      g_running = g_loading, g_started_ms = now;
      g_loading.clear();
    } else if (now - g_loading_ms > kLoadTimeoutMs) {
      say(std::format("{} didn't load; skipped", g_loading));
      mark_done(g_loading);
      g_loading.clear();
    }
  }
  if (!g_running.empty() && now - g_started_ms > 2000 && !running(g_running)) {
    say(std::format("{} ended; Ctrl+Shift+M starts the next", g_running));
    g_running.clear();
    g_dirty = true;
  }
}

} // namespace

void configure(const std::filesystem::path& plugin_dir, unsigned script_stack) {
  g_plugin_dir = plugin_dir / "iv-coop";
  g_dir = g_plugin_dir / "coverage";
  g_stack = script_stack;
  load();
}

void register_hooks() {
  for (const Native& nat : kNatives) {
    const char* category = nat.category;
    native_hooks::on(nat.hash, nat.name, [category](const Call& c) { on_call(c, category); });
  }
}

void tick() {
  launcher_tick();
  const std::uint32_t now = GetTickCount();
  if (g_dirty && now - g_last_flush_ms > kFlushEveryMs) {
    g_last_flush_ms = now;
    flush();
  }
}

} // namespace ivc::story::coverage
