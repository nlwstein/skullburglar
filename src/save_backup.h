// Save backup at startup and the crash archive (B-065). Plain C++ with no SDK dependency, so tests/save_backup_test.cpp
// runs it on its own.
//
// Next to the user data root (the folder with the saves, profile and bots.toml) the app keeps two folders of
// timestamped copies, newest 10 of each:
//   save-backups/<yyyymmdd-HHMMSS>/   everything in the user data root except the shader cache, copied before the
//                                     game starts. The live save is only ever read.
//   crashes/<yyyymmdd-HHMMSS>/        the crash dump (Windows minidump, or the host backtrace elsewhere) and the log
//                                     of a session that crashed.
// A root called "user" (the layout of the old play.bat: run\user) uses the plain names beside it. Any other root, the
// per-user default among them, uses "<name>-save-backups" and "<name>-crashes", so nothing generic lands in a shared
// folder such as Documents.
// A backup problem is logged by the caller and never stops the launch.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "path_utf8.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bots::savebackup {

constexpr size_t kKeep = 10;  // backups and crash archives kept

// "20261103-141503": the folder name of a backup or crash archive. Names in this form sort by time.
inline std::string Timestamp(std::time_t t) {
  char buf[32];
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

// True for exactly the form Timestamp() makes. Pruning touches nothing else.
inline bool IsStamp(const std::string& name) {
  if (name.size() != 15 || name[8] != '-') return false;
  for (size_t i = 0; i < name.size(); ++i)
    if (i != 8 && !std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  return true;
}

// The folder for backups ("save-backups") or crash archives ("crashes") belonging to a user data root.
inline std::filesystem::path SiblingDir(std::filesystem::path root, const std::string& leaf) {
  root = root.lexically_normal();
  if (!root.has_filename()) root = root.parent_path();
  const std::string name = bots::PathUtf8(root.filename());
  const std::filesystem::path parent = root.parent_path();
  if (name == "user" || name.empty()) return parent / leaf;
  return parent / bots::PathFromUtf8(name + "-" + leaf);
}

// The names to delete so that `keep` remain: the oldest timestamp-named ones. Anything else is left alone.
inline std::vector<std::string> PruneList(std::vector<std::string> names, size_t keep = kKeep) {
  names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsStamp(n); }),
              names.end());
  std::sort(names.begin(), names.end());
  std::vector<std::string> out;
  if (names.size() > keep) out.assign(names.begin(), names.end() - static_cast<std::ptrdiff_t>(keep));
  return out;
}

// The shader cache sits inside the default user data root, is large and rebuilds by itself: never backed up.
inline bool IsExcluded(const std::string& top_level_name) { return top_level_name == "cache"; }

// True when the root holds something worth keeping: a file outside the cache other than the settings file. A root with
// only bots.toml (settings, no progress) or only a cache is not backed up, so empty backups don't push real ones out.
inline bool HasSaves(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return false;
  for (const auto& top : fs::directory_iterator(root, ec)) {
    const std::string name = bots::PathUtf8(top.path().filename());
    if (IsExcluded(name)) continue;
    if (top.is_directory(ec)) {
      for (fs::recursive_directory_iterator it(top.path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) return true;
      ec.clear();
    } else if (top.is_regular_file(ec) && name != "bots.toml") {
      return true;
    }
  }
  return false;
}

// Deletes all but the newest `keep` timestamp-named folders in `dir`.
inline void Prune(const std::filesystem::path& dir, size_t keep = kKeep) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec)) names.push_back(bots::PathUtf8(e.path().filename()));
  for (const auto& n : PruneList(names, keep)) fs::remove_all(dir / n, ec);
}

// Copies the user data root (minus the cache) to backup_dir/<stamp>. Returns the new folder, or empty when there was
// nothing to back up, a backup with that stamp exists, or the copy failed (error is then set). The copy goes to
// "<stamp>.partial" first and is renamed when complete, so a half copy never counts as a backup. The root is only read.
inline std::filesystem::path BackUp(const std::filesystem::path& root, const std::filesystem::path& backup_dir,
                                    const std::string& stamp, std::string* error = nullptr, size_t keep = kKeep) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!HasSaves(root)) return {};
  const fs::path final_dir = backup_dir / stamp;
  const fs::path partial = backup_dir / (stamp + ".partial");
  if (fs::exists(final_dir, ec)) return {};
  fs::remove_all(partial, ec);
  fs::create_directories(partial, ec);
  if (ec) {
    if (error) *error = "can't create " + bots::PathUtf8(partial) + ": " + ec.message();
    return {};
  }
  for (const auto& top : fs::directory_iterator(root, ec)) {
    if (IsExcluded(bots::PathUtf8(top.path().filename()))) continue;
    std::error_code cec;
    fs::copy(top.path(), partial / top.path().filename(),
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, cec);
    if (cec) {
      if (error) *error = "copying " + bots::PathUtf8(top.path().filename()) + ": " + cec.message();
      fs::remove_all(partial, ec);
      return {};
    }
  }
  fs::rename(partial, final_dir, ec);
  if (ec) {
    if (error) *error = "can't finish " + bots::PathUtf8(final_dir) + ": " + ec.message();
    fs::remove_all(partial, ec);
    return {};
  }
  Prune(backup_dir, keep);
  return final_dir;
}

// The log of this session when --log_file isn't given: the SDK numbers its logs "<name>_NNN.log" in the log folder and
// the newest number is the one it just opened. Empty when there is none.
inline std::filesystem::path NewestLog(const std::filesystem::path& log_dir, const std::string& prefix) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string best;
  for (const auto& e : fs::directory_iterator(log_dir, ec)) {
    const std::string n = bots::PathUtf8(e.path().filename());
    if (n.size() > prefix.size() + 4 && n.compare(0, prefix.size(), prefix) == 0 && n.ends_with(".log") && n > best)
      best = n;  // equal-width numbers sort by value
  }
  return best.empty() ? fs::path() : log_dir / bots::PathFromUtf8(best);
}

// Copies the given files into crash_dir/<stamp> (those that exist). Returns how many were copied.
inline int ArchiveCrash(const std::vector<std::filesystem::path>& files, const std::filesystem::path& crash_dir,
                        const std::string& stamp) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dest = crash_dir / stamp;
  fs::create_directories(dest, ec);
  int n = 0;
  for (const auto& f : files) {
    if (!fs::is_regular_file(f, ec)) continue;
    fs::copy_file(f, dest / f.filename(), fs::copy_options::overwrite_existing, ec);
    if (!ec) ++n;
  }
  return n;
}

#if !defined(_WIN32)
// The crash path on macOS and Linux runs inside a signal handler, so it uses only open, read, write, mkdir and close,
// on paths prepared at startup (Arm). Arm() sets where the next crash goes; nothing is created until one happens.
struct CrashTarget {
  char dir[1024] = {};      // crashes/<stamp>
  char parent[1024] = {};   // crashes
  char log_src[1024] = {};  // the log of this session
  bool armed = false;
};
inline CrashTarget& Target() {
  static CrashTarget t;
  return t;
}
inline bool CopyToBuf(char* dst, size_t cap, const std::string& s) {
  if (s.size() + 1 > cap) return false;
  std::snprintf(dst, cap, "%s", s.c_str());
  return true;
}
inline void Arm(const std::filesystem::path& crash_dir, const std::string& stamp, const std::filesystem::path& log) {
  CrashTarget& t = Target();
  t.armed = CopyToBuf(t.parent, sizeof(t.parent), crash_dir.string()) &&
            CopyToBuf(t.dir, sizeof(t.dir), (crash_dir / stamp).string()) &&
            CopyToBuf(t.log_src, sizeof(t.log_src), log.string());
}
// Creates crashes/<stamp> and returns an open file descriptor for the backtrace in it (-1: not armed or failed).
inline int OpenBacktrace() {
  CrashTarget& t = Target();
  if (!t.armed) return -1;
  mkdir(t.parent, 0755);
  mkdir(t.dir, 0755);
  char path[1100];
  std::snprintf(path, sizeof(path), "%s/backtrace.txt", t.dir);
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
// Copies the session's log into crashes/<stamp> (the part written to disk so far).
inline void CopyLog() {
  CrashTarget& t = Target();
  if (!t.armed || !t.log_src[0]) return;
  const char* base = t.log_src;
  for (const char* p = t.log_src; *p; ++p)
    if (*p == '/') base = p + 1;
  char path[1200];
  std::snprintf(path, sizeof(path), "%s/%s", t.dir, base);
  const int in = open(t.log_src, O_RDONLY);
  if (in < 0) return;
  const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out >= 0) {
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
      if (write(out, buf, static_cast<size_t>(n)) != n) break;
    close(out);
  }
  close(in);
}
#endif

}  // namespace bots::savebackup
