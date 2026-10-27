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

#include "core/log.h"
#include "game/native_hooks.h"

namespace ivc::story::coverage {
namespace {
using native_hooks::Call;

struct Native {
  std::uint32_t hash;
  const char* name;
  const char* category;
};
constexpr Native kNatives[] = {
#include "story/coverage_natives.inc"
};

constexpr std::uint32_t kFlushEveryMs = 30000;
constexpr std::size_t kMaxSamples = 6;

struct Stat {
  std::string category;
  std::uint64_t count = 0;
  std::set<std::string> samples;
};
using Key = std::pair<std::string, std::string>; // script, native

std::filesystem::path g_dir;
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

} // namespace

void configure(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir / "iv-coop" / "coverage";
  load();
}

void register_hooks() {
  for (const Native& nat : kNatives) {
    const char* category = nat.category;
    native_hooks::on(nat.hash, nat.name, [category](const Call& c) { on_call(c, category); });
  }
}

void tick() {
  const std::uint32_t now = GetTickCount();
  if (g_dirty && now - g_last_flush_ms > kFlushEveryMs) {
    g_last_flush_ms = now;
    flush();
  }
}

} // namespace ivc::story::coverage
