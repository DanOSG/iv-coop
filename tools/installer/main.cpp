// iv-coop-setup: one-click installer for GTA IV co-op.
//
//   1. finds GTA IV (Steam registry + libraryfolders.vdf, or a folder picker),
//   2. checks GTAIV.exe is 1.2.0.59 and the game isn't running,
//   3. installs TJGM's Drag n' Drop Archive (FusionFix, radio restoration, fixes, HD
//      textures) from its official Mega mirror unless it's already installed, backing up
//      every file it would overwrite,
//   4. installs iv-coop.asi (embedded in this exe),
//   5. Uninstall removes the mod's files.
// Third-party files are fetched from their official mirror at install time, never bundled.

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/log_export.h"
#include "resource.h"

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ configuration

#define IVCOOP_WIDEN_(x) L##x
#define IVCOOP_WIDEN(x) IVCOOP_WIDEN_(x)
#define IVCOOP_WVERSION IVCOOP_WIDEN(IVCOOP_VERSION)
constexpr wchar_t kTitle[] =L"GTA IV Co-op " IVCOOP_WVERSION L" Setup";
constexpr const char* kPackFolder = "Mu4jhQia";            // TJGM's Drag n' Drop Archive, Mega mirror
constexpr const char* kPackKey = "h1uReUUe1JVtdNtjGZoEqw";
constexpr const char* kPackName = "TJGMsDragnDropArchive";  // part of the zip's file name
constexpr std::uint32_t kGameVersion[4] = {1, 2, 0, 59};
// TJGM's other official mirror. It sits behind a browser check, so it's offered to the
// user (download in the browser, then pick the zip) when Mega fails.
constexpr wchar_t kPackMediafire[] =
    L"https://www.mediafire.com/file/n0zgiw6d6sxrb4y/IV.TJGMsDragnDropArchive_v1.0.0.zip/file";
constexpr wchar_t kPackPage[] = L"https://tjgm.dev/resources/gtaiv/TJGM%27s-Drag-n%27-Drop-Archive/";
// Tomasak's Radio Restoration Mod (GPL-3.0; the radio restoration in TJGM's pack, same audio data), from its
// official GitHub release. Only its option files are used: "vanilla" for GTA IV's stations plus the "split"
// set, which gives the Episodes their own station lists (EFLC_SOUNDS/eflc_GAME and the episode setup files),
// so GTA IV's Vladivostok FM plays the original songs and the Episodes keep their club music.
constexpr wchar_t kRadioRelease[] =
    L"https://github.com/Tomasak/GTA-Downgraders/releases/download/iv-latest/Radio.Restoration.Mod.23-05-2025.rar";
struct RadioOption {
  const char* file; // inside the release, under "Resources/Radio Restorer/"
  std::uint32_t crc; // its hashes.ini CRC-32
};
constexpr RadioOption kRadioSplit[] = {{"opVANILLA.dat", 0x5C3C23F6}, {"opSPLITbase.dat", 0xDF4B1508},
                                       {"opSPLITVANILLA.dat", 0x54ECD144}};

// ------------------------------------------------------------------ UI plumbing

HWND g_wnd, g_log, g_progress, g_path_label, g_install, g_uninstall, g_browse, g_use_zip, g_split_radio, g_logs;
bool g_want_split_radio = false;
fs::path g_local_zip; // a pack zip the user downloaded themselves
fs::path g_game; // ...\Grand Theft Auto IV\GTAIV
bool g_busy = false;
constexpr UINT WM_LOG = WM_APP + 1, WM_PROGRESS = WM_APP + 2, WM_DONE = WM_APP + 3;

void log(const std::wstring& line) {
  PostMessageW(g_wnd, WM_LOG, 0, reinterpret_cast<LPARAM>(new std::wstring(line + L"\r\n")));
}
void log(const std::string& line) { log(std::wstring(line.begin(), line.end())); }
void progress(double fraction) {
  PostMessageW(g_wnd, WM_PROGRESS, static_cast<WPARAM>(fraction * 1000), 0);
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

struct Failure {
  std::wstring why;
};

// ------------------------------------------------------------------ finding the game

std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value) {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

std::vector<fs::path> steam_libraries() {
  std::vector<fs::path> libs;
  for (auto [root, key, value] : {std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
                                  std::tuple{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"},
                                  std::tuple{HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"}}) {
    const std::wstring steam = reg_string(root, key, value);
    if (steam.empty()) continue;
    libs.push_back(steam);
    std::ifstream vdf(fs::path(steam) / "steamapps" / "libraryfolders.vdf");
    for (std::string line; std::getline(vdf, line);) {
      const auto k = line.find("\"path\"");
      if (k == std::string::npos) continue;
      const auto a = line.find('"', k + 6), b = line.rfind('"');
      if (a == std::string::npos || b <= a) continue;
      std::string p = line.substr(a + 1, b - a - 1);
      for (std::size_t i; (i = p.find("\\\\")) != std::string::npos;) p.erase(i, 1);
      libs.push_back(widen(p));
    }
  }
  return libs;
}

std::optional<fs::path> find_game() {
  for (const auto& lib : steam_libraries()) {
    const fs::path game = lib / "steamapps" / "common" / "Grand Theft Auto IV" / "GTAIV";
    std::error_code ec;
    if (fs::exists(game / "GTAIV.exe", ec)) return game;
  }
  return std::nullopt;
}

std::optional<std::array<std::uint32_t, 4>> file_version(const fs::path& exe) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
  if (!size) return std::nullopt;
  std::vector<std::uint8_t> data(size);
  if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return std::nullopt;
  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return std::nullopt;
  return std::array<std::uint32_t, 4>{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                      HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

bool game_running() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe{sizeof(pe)};
  bool running = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (_wcsicmp(pe.szExeFile, L"GTAIV.exe") == 0) running = true;
  CloseHandle(snap);
  return running;
}

bool pack_installed(const fs::path& game) {
  std::error_code ec;
  return fs::exists(game / "plugins" / "GTAIV.EFLC.FusionFix.asi", ec) && fs::exists(game / "dinput8.dll", ec) &&
         fs::exists(game / "update" / "Various Fixes", ec) &&
         fs::exists(game / "update" / "pc" / "audio" / "sfx" / "RADIO_RESTORATION.rpf", ec);
}

// ------------------------------------------------------------------ HTTP (WinHTTP)

struct Url {
  std::wstring host, path;
  INTERNET_PORT port;
  bool https;
};

Url crack(const std::wstring& url) {
  URL_COMPONENTS c{sizeof(c)};
  wchar_t host[256], path[4096];
  c.lpszHostName = host, c.dwHostNameLength = 256;
  c.lpszUrlPath = path, c.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c)) throw Failure{L"bad URL " + url};
  return {host, path, c.nPort, c.nScheme == INTERNET_SCHEME_HTTPS};
}

using Body = std::function<void(const std::uint8_t*, std::size_t)>;

// Performs a request and streams the response body to `on_body`. Returns the HTTP status.
DWORD http(const std::wstring& url, const char* method, const std::string& body, const Body& on_body,
           std::uint64_t* content_length = nullptr) {
  const Url u = crack(url);
  struct H {
    HINTERNET h;
    ~H() { if (h) WinHttpCloseHandle(h); }
  };
  H session{WinHttpOpen(L"iv-coop-setup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
  if (!session.h) throw Failure{L"no internet session"};
  H connect{WinHttpConnect(session.h, u.host.c_str(), u.port, 0)};
  if (!connect.h) throw Failure{L"can't reach " + u.host};
  H req{WinHttpOpenRequest(connect.h, widen(method).c_str(), u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)};
  if (!req.h) throw Failure{L"request failed"};
  const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json";
  if (!WinHttpSendRequest(req.h, headers, body.empty() ? 0 : static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
                          static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
      !WinHttpReceiveResponse(req.h, nullptr))
    throw Failure{L"no response from " + u.host};
  DWORD status = 0, len = sizeof(status);
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
  if (content_length) {
    wchar_t buf[32];
    DWORD blen = sizeof(buf);
    *content_length = WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, buf, &blen, nullptr)
                          ? _wcstoui64(buf, nullptr, 10)
                          : 0;
  }
  std::vector<std::uint8_t> chunk(1 << 20);
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail)) throw Failure{L"download interrupted"};
    if (!avail) break;
    while (avail) {
      DWORD got = 0;
      const DWORD want = std::min<DWORD>(avail, static_cast<DWORD>(chunk.size()));
      if (!WinHttpReadData(req.h, chunk.data(), want, &got) || !got) throw Failure{L"download interrupted"};
      on_body(chunk.data(), got);
      avail -= got;
    }
  }
  return status;
}

std::string http_text(const std::wstring& url, const std::string& body) {
  std::string out;
  const DWORD status = http(url, body.empty() ? "GET" : "POST", body,
                            [&](const std::uint8_t* p, std::size_t n) { out.append(reinterpret_cast<const char*>(p), n); });
  if (status != 200) throw Failure{L"server answered " + std::to_wstring(status)};
  return out;
}

// ------------------------------------------------------------------ minimal JSON

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;
struct Json {
  std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> v;
  const Json* get(const std::string& k) const {
    if (auto* o = std::get_if<JsonObject>(&v))
      if (auto it = o->find(k); it != o->end()) return &it->second;
    return nullptr;
  }
  std::string str() const { return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : ""; }
  double num() const { return std::holds_alternative<double>(v) ? std::get<double>(v) : 0; }
};

struct JsonParser {
  const std::string& s;
  std::size_t i = 0;
  void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
  Json parse() {
    ws();
    if (i >= s.size()) throw Failure{L"bad server reply"};
    const char c = s[i];
    if (c == '{') {
      ++i;
      JsonObject o;
      ws();
      if (s[i] == '}') return ++i, Json{o};
      for (;;) {
        ws();
        std::string k = parse().str();
        ws();
        ++i; // ':'
        o[k] = parse();
        ws();
        if (s[i++] == '}') return Json{o};
      }
    }
    if (c == '[') {
      ++i;
      JsonArray a;
      ws();
      if (s[i] == ']') return ++i, Json{a};
      for (;;) {
        a.push_back(parse());
        ws();
        if (s[i++] == ']') return Json{a};
      }
    }
    if (c == '"') {
      std::string out;
      for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
          ++i;
          if (s[i] == 'u') { out += '?'; i += 4; continue; } // not needed for our fields
          out += s[i] == 'n' ? '\n' : s[i];
        } else {
          out += s[i];
        }
      }
      ++i;
      return Json{out};
    }
    if (s.compare(i, 4, "true") == 0) return i += 4, Json{true};
    if (s.compare(i, 5, "false") == 0) return i += 5, Json{false};
    if (s.compare(i, 4, "null") == 0) return i += 4, Json{nullptr};
    std::size_t end = i;
    while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '-' || s[end] == '.' ||
                              s[end] == 'e' || s[end] == 'E' || s[end] == '+'))
      ++end;
    const double d = std::stod(s.substr(i, end - i));
    i = end;
    return Json{d};
  }
};

Json parse_json(const std::string& s) {
  JsonParser p{s};
  return p.parse();
}

// ------------------------------------------------------------------ AES (BCrypt)

std::vector<std::uint8_t> b64url(std::string s) {
  for (char& c : s) c = c == '-' ? '+' : c == '_' ? '/' : c;
  s.erase(std::remove(s.begin(), s.end(), ','), s.end());
  while (s.size() % 4) s += '=';
  static const std::string abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    const auto v = abc.find(c);
    if (v == std::string::npos) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v), bits += 6;
    if (bits >= 8) out.push_back(static_cast<std::uint8_t>(acc >> (bits -= 8)));
  }
  return out;
}

class Aes {
 public:
  Aes(const std::uint8_t* key, const wchar_t* mode) {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0) ||
        BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)),
                          static_cast<ULONG>((wcslen(mode) + 1) * 2), 0) ||
        BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, const_cast<PUCHAR>(key), 16, 0))
      throw Failure{L"AES unavailable"};
  }
  ~Aes() {
    if (key_) BCryptDestroyKey(key_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  void encrypt(std::uint8_t* data, std::size_t n) {
    ULONG out = 0;
    BCryptEncrypt(key_, data, static_cast<ULONG>(n), nullptr, nullptr, 0, data, static_cast<ULONG>(n), &out, 0);
  }
  void decrypt(std::uint8_t* data, std::size_t n, std::uint8_t* iv = nullptr) {
    ULONG out = 0;
    BCryptDecrypt(key_, data, static_cast<ULONG>(n), nullptr, iv, iv ? 16 : 0, data, static_cast<ULONG>(n), &out, 0);
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_KEY_HANDLE key_ = nullptr;
};

// ------------------------------------------------------------------ Mega public folder download

struct MegaFile {
  std::string name, handle;
  std::uint64_t size = 0;
  std::uint8_t key[16], nonce[8];
};

std::wstring mega_api(const char* folder) {
  return widen(std::string("https://g.api.mega.co.nz/cs?id=1&n=") + folder);
}

MegaFile mega_find(const char* folder, const char* key_b64, const char* name_part) {
  const auto master = b64url(key_b64);
  if (master.size() != 16) throw Failure{L"bad folder key"};
  const Json reply = parse_json(http_text(mega_api(folder), R"([{"a":"f","c":1,"r":1,"ca":1}])"));
  const auto* arr = std::get_if<JsonArray>(&reply.v);
  const Json* nodes = arr && !arr->empty() ? (*arr)[0].get("f") : nullptr;
  if (!nodes) throw Failure{L"the download mirror didn't list its files"};
  for (const Json& node : std::get<JsonArray>(nodes->v)) {
    if (!node.get("t") || node.get("t")->num() != 0 || !node.get("k")) continue;
    const std::string k = node.get("k")->str();
    auto raw = b64url(k.substr(k.find(':') + 1));
    if (raw.size() != 32) continue;
    Aes(master.data(), BCRYPT_CHAIN_MODE_ECB).decrypt(raw.data(), 32);
    MegaFile f;
    for (int i = 0; i < 16; ++i) f.key[i] = raw[i] ^ raw[16 + i];
    std::memcpy(f.nonce, raw.data() + 16, 8);
    auto attr = b64url(node.get("a")->str());
    std::uint8_t iv[16] = {};
    Aes(f.key, BCRYPT_CHAIN_MODE_CBC).decrypt(attr.data(), attr.size() & ~std::size_t{15}, iv);
    const std::string text(attr.begin(), attr.end());
    if (text.rfind("MEGA", 0) != 0) continue;
    f.name = parse_json(text.substr(4, text.find('\0') == std::string::npos ? std::string::npos : text.find('\0') - 4))
                 .get("n")->str();
    f.handle = node.get("h")->str();
    f.size = static_cast<std::uint64_t>(node.get("s")->num());
    if (f.name.find(name_part) != std::string::npos) return f;
  }
  throw Failure{L"the pack wasn't found on its download mirror"};
}

struct StopDownload {};
std::uint64_t g_download_limit = 0; // tests: stop after this many bytes (0 = all)

void mega_download(const char* folder, const MegaFile& f, const fs::path& out) {
  const Json reply = parse_json(http_text(mega_api(folder), R"([{"a":"g","g":1,"n":")" + f.handle + R"("}])"));
  const auto* arr = std::get_if<JsonArray>(&reply.v);
  const Json* g = arr && !arr->empty() ? (*arr)[0].get("g") : nullptr;
  if (!g || g->str().empty()) throw Failure{L"the mirror gave no download link (quota reached? try later)"};
  std::ofstream file(out, std::ios::binary);
  if (!file) throw Failure{L"can't write " + out.wstring()};
  Aes aes(f.key, BCRYPT_CHAIN_MODE_ECB);
  std::uint64_t done = 0;
  std::vector<std::uint8_t> stream; // keystream for the current chunk
  int last_pct = -1;
  const DWORD status = http(widen(g->str()), "GET", {}, [&](const std::uint8_t* p, std::size_t n) {
    // AES-CTR: counter block = nonce (8 bytes) || big-endian block index.
    const std::uint64_t first_block = done / 16, last_block = (done + n + 15) / 16;
    stream.resize(static_cast<std::size_t>(last_block - first_block) * 16);
    for (std::uint64_t b = first_block; b < last_block; ++b) {
      std::uint8_t* c = stream.data() + (b - first_block) * 16;
      std::memcpy(c, f.nonce, 8);
      for (int i = 0; i < 8; ++i) c[8 + i] = static_cast<std::uint8_t>(b >> (56 - 8 * i));
    }
    aes.encrypt(stream.data(), stream.size());
    std::vector<std::uint8_t> plain(p, p + n);
    const std::size_t skew = done % 16;
    for (std::size_t i = 0; i < n; ++i) plain[i] ^= stream[skew + i];
    file.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(n));
    done += n;
    if (g_download_limit && done >= g_download_limit) throw StopDownload{};
    const int pct = static_cast<int>(done * 100 / std::max<std::uint64_t>(f.size, 1));
    progress(0.05 + 0.75 * static_cast<double>(done) / static_cast<double>(std::max<std::uint64_t>(f.size, 1)));
    if (pct != last_pct && pct % 5 == 0) {
      last_pct = pct;
      log(L"  " + std::to_wstring(done >> 20) + L" / " + std::to_wstring(f.size >> 20) + L" MB");
    }
  });
  if (status != 200) throw Failure{L"the download failed (HTTP " + std::to_wstring(status) + L")"};
  if (done != f.size) throw Failure{L"the download was incomplete"};
}

// ------------------------------------------------------------------ processes, files

// Runs a command, returns its exit code; stdout lines go to `out` if given.
DWORD run(const std::wstring& cmd, std::vector<std::string>* out = nullptr) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (out && !CreatePipe(&rd, &wr, &sa, 0)) throw Failure{L"pipe failed"};
  if (rd) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW | (out ? STARTF_USESTDHANDLES : 0);
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr, si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::wstring line = cmd;
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, out != nullptr, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                      &pi))
    throw Failure{L"couldn't run: " + cmd};
  if (wr) CloseHandle(wr);
  if (out) {
    std::string buf;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(rd, chunk, sizeof(chunk), &got, nullptr) && got) buf.append(chunk, got);
    CloseHandle(rd);
    std::istringstream lines(buf);
    for (std::string l; std::getline(lines, l);) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      out->push_back(l);
    }
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return code;
}

// Windows' tar (bsdtar, reads zip). A 32-bit process must reach System32 through Sysnative.
fs::path tar_exe() {
  wchar_t win[MAX_PATH];
  GetWindowsDirectoryW(win, MAX_PATH);
  for (const wchar_t* sub : {L"Sysnative", L"System32"}) {
    const fs::path p = fs::path(win) / sub / "tar.exe";
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
  }
  throw Failure{L"Windows' tar.exe is missing (needs Windows 10 1803 or newer)"};
}

fs::path backup_dir(const fs::path& game) { return game / "_iv-coop_backup"; }

// Installs the pack from a zip: checks it is TJGM's archive, backs up every existing file
// it would overwrite (paths after its top folder), then extracts it over the game folder.
void extract_pack(const fs::path& game, const fs::path& zip) {
  const fs::path tar = tar_exe();
  log(L"Checking the archive...");
  std::vector<std::string> listing;
  if (run(L"\"" + tar.wstring() + L"\" -tf \"" + zip.wstring() + L"\"", &listing) != 0 || listing.empty())
    throw Failure{L"that archive is damaged or isn't a zip"};
  if (listing.front().find(kPackName) == std::string::npos)
    throw Failure{L"that zip isn't TJGM's Drag n' Drop Archive"};
  const fs::path backup = backup_dir(game);
  std::ofstream manifest(backup / "manifest.txt", std::ios::app);
  int backed_up = 0;
  for (const std::string& entry : listing) {
    const auto slash = entry.find('/');
    if (slash == std::string::npos || entry.back() == '/') continue;
    const fs::path rel = fs::path(widen(entry.substr(slash + 1))).make_preferred();
    const fs::path target = game / rel;
    std::error_code ec;
    if (!fs::exists(target, ec) || fs::exists(backup / rel, ec)) continue;
    fs::create_directories((backup / rel).parent_path());
    fs::copy_file(target, backup / rel, ec);
    manifest << "pack-overwrote " << rel.string() << std::endl;
    ++backed_up;
  }
  log(L"Backed up " + std::to_wstring(backed_up) + L" file(s) the pack replaces.");
  log(L"Installing the pack...");
  if (run(L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" --strip-components=1 -C \"" + game.wstring() +
          L"\"") != 0)
    throw Failure{L"extracting the pack failed"};
  progress(0.95);
}

std::optional<fs::path> pick_zip(HWND owner) {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"TJGM's Drag n' Drop Archive (*.zip)\0*.zip\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"Pick the downloaded IV.TJGMsDragnDropArchive zip";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) return std::nullopt;
  return fs::path(file);
}

// Mega failed: the user downloads from TJGM's Mediafire mirror in their browser.
fs::path fallback_zip(const std::wstring& why) {
  const std::wstring text =
      L"The automatic download from TJGM's Mega mirror failed:\n" + why +
      L"\n\nYou can download the pack from TJGM's Mediafire mirror in your browser instead, then pick the "
      L"downloaded zip here.\n\nOpen the Mediafire download page now?";
  if (MessageBoxW(g_wnd, text.c_str(), kTitle, MB_YESNO | MB_ICONWARNING) != IDYES) throw Failure{why};
  ShellExecuteW(nullptr, L"open", kPackMediafire, nullptr, nullptr, SW_SHOWNORMAL);
  MessageBoxW(g_wnd,
              L"When the download has finished, click OK and pick the zip (IV.TJGMsDragnDropArchive v1.0.0.zip).",
              kTitle, MB_OK | MB_ICONINFORMATION);
  if (auto zip = pick_zip(g_wnd)) return *zip;
  throw Failure{L"no zip picked"};
}

void install_pack(const fs::path& game) {
  if (!g_local_zip.empty()) { // the user's own download: install from it, leave it in place
    log(L"Using " + g_local_zip.wstring());
    extract_pack(game, g_local_zip);
    return;
  }
  const fs::path dl = game / "_iv-coop_download";
  const fs::path zip = dl / "pack.zip";
  try {
    log(L"Finding TJGM's Drag n' Drop Archive on its Mega mirror...");
    const MegaFile f = mega_find(kPackFolder, kPackKey, kPackName);
    log(widen(f.name) + L" (" + std::to_wstring(f.size >> 20) + L" MB)");
    ULARGE_INTEGER free{};
    GetDiskFreeSpaceExW(game.c_str(), &free, nullptr, nullptr);
    if (free.QuadPart < f.size * 2 + (512ull << 20))
      throw Failure{L"not enough disk space: about " + std::to_wstring((f.size * 2) >> 30) + L" GB needed"};
    fs::create_directories(dl);
    log(L"Downloading (this takes a while)...");
    mega_download(kPackFolder, f, zip);
  } catch (const Failure& f) {
    std::error_code ec;
    fs::remove_all(dl, ec);
    if (f.why.rfind(L"not enough disk space", 0) == 0) throw;
    log(L"Mega download failed: " + f.why);
    const fs::path picked = fallback_zip(f.why);
    log(L"Using " + picked.wstring());
    extract_pack(game, picked);
    return;
  }
  progress(0.82);
  extract_pack(game, zip);
  std::error_code ec;
  fs::remove_all(dl, ec);
}

std::uint32_t crc32_file(const fs::path& path) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::ifstream in(path, std::ios::binary);
  std::uint32_t c = 0xFFFFFFFFu;
  std::vector<char> buf(1 << 16);
  while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount())
    for (std::streamsize i = 0; i < in.gcount(); ++i)
      c = table[(c ^ static_cast<std::uint8_t>(buf[static_cast<std::size_t>(i)])) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// The split radio: downloads the Radio Restoration release (~1 GB), takes its three option files (checked
// against the release's own CRCs), backs up what they replace, and unpacks them over the game folder.
// `local_rar`: an already downloaded release (tests); otherwise it is downloaded.
void install_split_radio(const fs::path& game, const fs::path& local_rar = {}) {
  const fs::path dl = game / "_iv-coop_download";
  const fs::path rar = local_rar.empty() ? dl / "radio-restoration.rar" : local_rar;
  fs::create_directories(dl);
  if (local_rar.empty()) {
    log(L"Downloading Tomasak's Radio Restoration release from GitHub (about 1 GB)...");
    std::ofstream out(rar, std::ios::binary | std::ios::trunc);
    std::uint64_t total = 0, done = 0;
    const DWORD status = http(kRadioRelease, "GET", "", [&](const std::uint8_t* p, std::size_t n) {
      out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
      done += n;
      if (total) progress(0.85 + 0.1 * static_cast<double>(done) / static_cast<double>(total));
    }, &total);
    if (status != 200 || !out) throw Failure{L"the Radio Restoration download failed (HTTP " + std::to_wstring(status) + L")"};
  }
  const fs::path tar = tar_exe();
  std::wstring names;
  for (const RadioOption& o : kRadioSplit) names += L" \"Resources/Radio Restorer/" + widen(o.file) + L"\"";
  if (run(L"\"" + tar.wstring() + L"\" -xf \"" + rar.wstring() + L"\" -C \"" + dl.wstring() + L"\"" + names) != 0)
    throw Failure{L"couldn't unpack the Radio Restoration release"};
  const fs::path backup = backup_dir(game);
  std::ofstream manifest(backup / "manifest.txt", std::ios::app);
  for (const RadioOption& o : kRadioSplit) {
    const fs::path opt = dl / "Resources" / "Radio Restorer" / o.file;
    if (crc32_file(opt) != o.crc) throw Failure{L"the Radio Restoration file " + widen(o.file) + L" didn't check out"};
    std::vector<std::string> listing;
    if (run(L"\"" + tar.wstring() + L"\" -tf \"" + opt.wstring() + L"\"", &listing) != 0)
      throw Failure{L"couldn't read " + widen(o.file)};
    for (const std::string& entry : listing) {
      if (entry.empty() || entry.back() == '/' || entry.back() == '\\') continue;
      const fs::path rel = fs::path(widen(entry)).make_preferred();
      std::error_code ec;
      if (!fs::exists(game / rel, ec) || fs::exists(backup / rel, ec)) continue;
      fs::create_directories((backup / rel).parent_path());
      fs::copy_file(game / rel, backup / rel, ec);
      manifest << "radio-overwrote " << rel.string() << std::endl;
    }
    if (run(L"\"" + tar.wstring() + L"\" -xf \"" + opt.wstring() + L"\" -C \"" + game.wstring() + L"\"") != 0)
      throw Failure{L"installing " + widen(o.file) + L" failed"};
  }
  std::error_code ec;
  fs::remove_all(dl, ec);
  log(L"Radio: GTA IV's stations play the original songs, the Episodes keep their own (Radio Restoration "
      L"\"split\", by Tomasak).");
}

void install_mod(const fs::path& game) {
  HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_IVCOOP_ASI), RT_RCDATA);
  HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
  if (!data) throw Failure{L"this installer is missing the mod (bad build)"};
  const auto* bytes = static_cast<const char*>(LockResource(data));
  const DWORD size = SizeofResource(nullptr, res);
  fs::create_directories(game / "plugins");
  std::ofstream out(game / "plugins" / "iv-coop.asi", std::ios::binary | std::ios::trunc);
  out.write(bytes, size);
  if (!out) throw Failure{L"couldn't write plugins\\iv-coop.asi"};
  log(L"Installed plugins\\iv-coop.asi, version " IVCOOP_WVERSION L" ("+ std::to_wstring(size >> 10) + L" KB).");
}

void do_install() {
  const fs::path game = g_game;
  log(L"Game folder: " + game.wstring());
  const auto v = file_version(game / "GTAIV.exe");
  if (!v) throw Failure{L"GTAIV.exe not found in that folder"};
  if (!std::equal(v->begin(), v->end(), kGameVersion))
    throw Failure{L"GTAIV.exe is version " + std::to_wstring((*v)[0]) + L"." + std::to_wstring((*v)[1]) + L"." +
                  std::to_wstring((*v)[2]) + L"." + std::to_wstring((*v)[3]) +
                  L"; co-op needs the current Complete Edition (1.2.0.59)"};
  if (game_running()) throw Failure{L"close GTA IV first"};
  fs::create_directories(backup_dir(game));
  progress(0.05);
  if (pack_installed(game) && g_local_zip.empty())
    log(L"TJGM's pack (FusionFix, radio restoration, fixes) is already installed; skipping it.");
  else
    install_pack(game);
  if (g_want_split_radio) install_split_radio(game);
  install_mod(game);
  progress(1.0);
  log(L"Done. Start GTA IV, then open the phone > Multiplayer to host or join.");
}

void do_uninstall() {
  const fs::path game = g_game;
  if (game_running()) throw Failure{L"close GTA IV first"};
  std::error_code ec;
  int removed = 0;
  for (const fs::path p : {game / "plugins" / "iv-coop.asi", game / "plugins" / "iv-coop.ini",
                           game / "plugins" / "iv-coop.log", game / "plugins" / "iv-coop.pdb"})
    removed += fs::remove(p, ec) ? 1 : 0;
  removed += static_cast<int>(fs::remove_all(game / "plugins" / "iv-coop", ec));
  log(L"Removed " + std::to_wstring(removed) + L" co-op file(s). TJGM's pack stays installed (it's a general "
      L"improvement); files it replaced are kept in _iv-coop_backup.");
  progress(1.0);
}

// ------------------------------------------------------------------ window

void set_busy(bool busy) {
  g_busy = busy;
  EnableWindow(g_install, !busy && !g_game.empty());
  EnableWindow(g_uninstall, !busy && !g_game.empty());
  EnableWindow(g_browse, !busy);
  EnableWindow(g_use_zip, !busy && !g_game.empty());
  EnableWindow(g_logs, !busy && !g_game.empty());
  EnableWindow(g_split_radio, !busy);
}

void show_game() {
  SetWindowTextW(g_path_label, g_game.empty() ? L"GTA IV not found: pick its GTAIV folder."
                                              : (L"GTA IV: " + g_game.wstring()).c_str());
  set_busy(false);
}

// Collect logs: the same zip the mod makes in game (phone: Multiplayer > Save logs), for when the game won't start
// or crashed. Shown in Explorer when done.
void do_collect_logs() {
  log(L"Collecting logs...");
  const ivc::log_export::Result r = ivc::log_export::run(g_game, IVCOOP_VERSION);
  log(r.message);
  if (r.ok) {
    const std::wstring args = L"/select,\"" + r.zip.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  }
}

void start(void (*job)()) {
  set_busy(true);
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  std::thread([job] {
    try {
      job();
    } catch (const Failure& f) {
      log(L"Stopped: " + f.why);
    } catch (const std::exception& e) {
      log(std::string("Stopped: ") + e.what());
    }
    PostMessageW(g_wnd, WM_DONE, 0, 0);
  }).detach();
}

void browse() {
  BROWSEINFOW bi{};
  bi.hwndOwner = g_wnd;
  bi.lpszTitle = L"Select GTA IV's GTAIV folder (the one with GTAIV.exe)";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
      fs::path p = path;
      std::error_code ec;
      if (!fs::exists(p / "GTAIV.exe", ec) && fs::exists(p / "GTAIV" / "GTAIV.exe", ec)) p /= "GTAIV";
      g_game = fs::exists(p / "GTAIV.exe", ec) ? p : fs::path();
      if (g_game.empty()) MessageBoxW(g_wnd, L"That folder has no GTAIV.exe.", kTitle, MB_ICONWARNING);
    }
    CoTaskMemFree(pidl);
  }
  show_game();
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
      g_want_split_radio = SendMessageW(g_split_radio, BM_GETCHECK, 0, 0) == BST_CHECKED;
      if (reinterpret_cast<HWND>(lp) == g_install) start(do_install);
      if (reinterpret_cast<HWND>(lp) == g_uninstall) start(do_uninstall);
      if (reinterpret_cast<HWND>(lp) == g_browse) browse();
      if (reinterpret_cast<HWND>(lp) == g_logs) start(do_collect_logs);
      if (reinterpret_cast<HWND>(lp) == g_use_zip)
        if (auto zip = pick_zip(h)) {
          g_local_zip = *zip;
          start(do_install);
        }
      return 0;
    case WM_LOG: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
      const int len = GetWindowTextLengthW(g_log);
      SendMessageW(g_log, EM_SETSEL, len, len);
      SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text->c_str()));
      return 0;
    }
    case WM_PROGRESS:
      SendMessageW(g_progress, PBM_SETPOS, wp, 0);
      return 0;
    case WM_DONE:
      set_busy(false);
      return 0;
    case WM_CLOSE:
      if (g_busy && MessageBoxW(h, L"Setup is still working. Quit anyway?", kTitle, MB_YESNO | MB_ICONWARNING) != IDYES)
        return 0;
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

// --selftest <report.txt>: everything except downloading and writing to the game folder.
int selftest(const fs::path& report) {
  std::wofstream out(report);
  try {
    const auto game = find_game();
    out << L"game: " << (game ? game->wstring() : L"(not found)") << std::endl;
    if (game) {
      const auto v = file_version(*game / "GTAIV.exe");
      out << L"GTAIV.exe version: ";
      if (v) out << (*v)[0] << L"." << (*v)[1] << L"." << (*v)[2] << L"." << (*v)[3];
      out << std::endl << L"pack installed: " << (pack_installed(*game) ? L"yes" : L"no") << std::endl;
    }
    out << L"game running: " << (game_running() ? L"yes" : L"no") << std::endl;
    out << L"tar: " << tar_exe().wstring() << std::endl;
    const MegaFile f = mega_find(kPackFolder, kPackKey, kPackName);
    out << L"mirror: " << widen(f.name) << L", " << f.size << L" bytes" << std::endl;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_IVCOOP_ASI), RT_RCDATA);
    out << L"embedded mod: " << (res ? SizeofResource(nullptr, res) : 0) << L" bytes" << std::endl << L"OK" << std::endl;
    return 0;
  } catch (const Failure& f) {
    out << L"FAILED: " << f.why << std::endl;
  } catch (const std::exception& e) {
    out << L"FAILED: " << widen(e.what()) << std::endl;
  }
  return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
  {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmd && *cmd ? GetCommandLineW() : L"x", &argc);
    if (argc >= 3 && std::wstring(argv[1]) == L"--selftest") return selftest(argv[2]);
    // --testdownload <out> <MB>: the first MB of the pack, through the real download path.
    if (argc >= 4 && std::wstring(argv[1]) == L"--testdownload") {
      g_download_limit = _wtoi64(argv[3]) << 20;
      try {
        mega_download(kPackFolder, mega_find(kPackFolder, kPackKey, kPackName), argv[2]);
      } catch (const StopDownload&) {
        return 0;
      } catch (...) {
        return 2;
      }
      return 0;
    }
    // --logs <game> <report>: Collect logs without the window.
    if (argc >= 4 && std::wstring(argv[1]) == L"--logs") {
      const ivc::log_export::Result r = ivc::log_export::run(argv[2], IVCOOP_VERSION);
      std::ofstream(argv[3]) << r.message << '\n' << r.zip.string() << '\n';
      return r.ok ? 0 : 2;
    }
    // --radiosplit <game> <release.rar> <report>: the split-radio step against a folder, from a local release.
    if (argc >= 5 && std::wstring(argv[1]) == L"--radiosplit") {
      std::wofstream report(argv[4]);
      try {
        fs::create_directories(backup_dir(argv[2]));
        install_split_radio(argv[2], argv[3]);
        report << L"ok";
        return 0;
      } catch (const Failure& f) {
        report << L"failed: " << f.why;
        return 2;
      }
    }
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"IvCoopSetup";
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassW(&wc);
  g_wnd = CreateWindowW(wc.lpszClassName, kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        CW_USEDEFAULT, CW_USEDEFAULT, 620, 440, nullptr, nullptr, inst, nullptr);
  const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int hgt) {
    HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, hgt, g_wnd, nullptr, inst, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return c;
  };
  make(L"STATIC",
       L"Installs GTA IV co-op, plus TJGM's Drag n' Drop Archive (FusionFix, radio restoration, fixes, HD textures) "
       L"if it isn't installed yet. Third-party files come from their official mirror.",
       0, 12, 10, 580, 34);
  g_path_label = make(L"STATIC", L"", SS_PATHELLIPSIS, 12, 52, 480, 18);
  g_browse = make(L"BUTTON", L"Change...", BS_PUSHBUTTON, 500, 48, 92, 24);
  g_install = make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, 12, 80, 120, 30);
  g_uninstall = make(L"BUTTON", L"Uninstall", BS_PUSHBUTTON, 140, 80, 120, 30);
  g_use_zip = make(L"BUTTON", L"Install from a downloaded zip...", BS_PUSHBUTTON, 392, 80, 200, 30);
  g_logs = make(L"BUTTON", L"Collect logs", BS_PUSHBUTTON, 268, 80, 116, 30);
  g_split_radio = make(L"BUTTON",
                       L"Original Vladivostok FM songs in GTA IV, club music only in the Episodes (Radio Restoration "
                       L"\"split\", ~1 GB download)",
                       BS_AUTOCHECKBOX | BS_MULTILINE, 12, 116, 580, 30);
  g_progress = make(PROGRESS_CLASSW, L"", 0, 12, 150, 580, 18);
  SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
  g_log = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 176, 580, 210);
  if (auto game = find_game()) g_game = *game;
  show_game();
  ShowWindow(g_wnd, show);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
