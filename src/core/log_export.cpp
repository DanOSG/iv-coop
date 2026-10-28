#include "core/log_export.h"

#include <windows.h>

#include <shlobj.h>

#include <format>
#include <fstream>
#include <system_error>
#include <vector>

namespace ivc::log_export {
namespace {
namespace fs = std::filesystem;

fs::path desktop() {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  if (out.empty()) out = fs::temp_directory_path();
  return out;
}

void copy_if(const fs::path& from, const fs::path& to_dir, std::vector<std::string>& listed) {
  std::error_code ec;
  if (!fs::is_regular_file(from, ec)) return;
  fs::create_directories(to_dir, ec);
  // Read it ourselves, sharing everything: the running game holds its log open for writing.
  const HANDLE in = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (in == INVALID_HANDLE_VALUE) return;
  std::ofstream out(to_dir / from.filename(), std::ios::binary);
  char buf[65536];
  DWORD got = 0;
  while (ReadFile(in, buf, sizeof(buf), &got, nullptr) && got) out.write(buf, got);
  CloseHandle(in);
  listed.push_back(from.filename().string());
}

bool run_tar(const fs::path& zip, const fs::path& dir, std::string& error) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  const fs::path tar = fs::path(sys) / L"tar.exe";
  std::wstring cmd = std::format(L"\"{}\" -a -c -f \"{}\" -C \"{}\" .", tar.wstring(), zip.wstring(), dir.wstring());
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = std::format("couldn't run tar.exe (error {})", GetLastError());
    return false;
  }
  WaitForSingleObject(pi.hProcess, 60000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) error = std::format("tar.exe failed (exit {})", code);
  return code == 0;
}

} // namespace

Result run(const fs::path& game_dir, const std::string& mod_version) {
  Result res;
  SYSTEMTIME t;
  GetLocalTime(&t);
  const std::string stamp =
      std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  std::error_code ec;
  const fs::path work = fs::temp_directory_path(ec) / ("iv-coop-logs-" + stamp);
  fs::create_directories(work, ec);
  if (ec) {
    res.message = "Couldn't make a temporary folder: " + ec.message();
    return res;
  }
  const fs::path plugins = game_dir / "plugins", mod = plugins / "iv-coop";
  std::vector<std::string> listed;
  for (const auto& e : fs::directory_iterator(plugins, ec)) {
    const std::string name = e.path().filename().string();
    if (name.starts_with("iv-coop") && (name.ends_with(".log") || name.ends_with(".ini")))
      copy_if(e.path(), work / "plugins", listed);
  }
  copy_if(plugins / "GTAIV.EFLC.FusionFix.ini", work / "plugins", listed);
  copy_if(game_dir / "GTAIV_d3d9.log", work, listed);
  for (const char* f : {"selftest_result.txt", "selftest_driver.log", "selftest_bot.log", "runner_result.txt",
                        "playlist_done.txt"})
    copy_if(mod / f, work / "iv-coop", listed);
  copy_if(mod / "coverage" / "coverage.md", work / "iv-coop", listed);
  copy_if(mod / "coverage" / "coverage.tsv", work / "iv-coop", listed);
  for (const auto& e : fs::directory_iterator(mod / "audit", ec))
    if (e.path().extension() == ".txt" || e.path().extension() == ".md") copy_if(e.path(), work / "iv-coop" / "audit", listed);
  {
    std::ofstream s(work / "summary.txt");
    s << "iv-coop " << mod_version << "\ncollected " << stamp << "\ngame folder " << game_dir.string() << "\n";
    std::error_code e2;
    s << "GTAIV.exe size " << fs::file_size(game_dir / "GTAIV.exe", e2) << "\nfiles:\n";
    for (const auto& n : listed) s << "  " << n << "\n";
  }
  const fs::path zip = desktop() / ("iv-coop-logs-" + stamp + ".zip");
  std::string error;
  const bool ok = run_tar(zip, work, error);
  fs::remove_all(work, ec);
  if (!ok) {
    res.message = "Couldn't make the zip: " + error;
    return res;
  }
  res.ok = true;
  res.zip = zip;
  res.message = std::format("Logs saved to your Desktop: {} ({} files)", zip.filename().string(), listed.size());
  return res;
}

} // namespace ivc::log_export
