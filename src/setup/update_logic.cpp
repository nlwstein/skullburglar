// See update_logic.h.
#include "update_logic.h"

#include "ed25519.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

namespace bots::setup {

namespace {

std::vector<long> VersionNumbers(const std::string& v) {
  std::vector<long> out;
  const char* p = v.c_str();
  if (*p == 'v') ++p;
  while (*p >= '0' && *p <= '9') {
    char* end;
    out.push_back(std::strtol(p, &end, 10));
    p = end;
    if (*p != '.') break;
    ++p;
  }
  return out;
}

// ---- A small JSON reader: enough for the releases file (objects, arrays, strings, numbers, true/false/null). ----
struct Json {
  enum Type { kNull, kBool, kNumber, kString, kArray, kObject } type = kNull;
  std::string str;
  double num = 0;
  bool b = false;
  std::vector<Json> items;                       // array
  std::vector<std::pair<std::string, Json>> kv;  // object

  const Json* Get(const std::string& key) const {
    if (type != kObject) return nullptr;
    for (const auto& [k, v] : kv)
      if (k == key) return &v;
    return nullptr;
  }
  std::string Str(const std::string& key) const {
    const Json* v = Get(key);
    return v && v->type == kString ? v->str : std::string();
  }
};

class Reader {
 public:
  explicit Reader(const std::string& s) : s_(s) {}
  bool Parse(Json& out) {
    if (!Value(out, 0)) return false;
    Space();
    return pos_ == s_.size();
  }

 private:
  void Space() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\t' || s_[pos_] == '\r')) ++pos_;
  }
  bool Lit(const char* word) {
    size_t n = std::strlen(word);
    if (s_.compare(pos_, n, word) != 0) return false;
    pos_ += n;
    return true;
  }
  static void Utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  bool String(std::string& out) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      char c = s_[pos_++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return false;
      char e = s_[pos_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (pos_ + 4 > s_.size()) return false;
          unsigned cp = unsigned(std::strtoul(s_.substr(pos_, 4).c_str(), nullptr, 16));
          pos_ += 4;
          Utf8(out, cp);  // surrogate pairs come out as two 3-byte sequences: fine for this file's ASCII keys
          break;
        }
        default: out += e;  // \" \\ \/
      }
    }
    if (pos_ >= s_.size()) return false;
    ++pos_;
    return true;
  }
  bool Value(Json& out, int depth) {
    if (depth > 32) return false;
    Space();
    if (pos_ >= s_.size()) return false;
    char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      out.type = Json::kObject;
      Space();
      if (pos_ < s_.size() && s_[pos_] == '}') return ++pos_, true;
      for (;;) {
        Space();
        std::string key;
        if (!String(key)) return false;
        Space();
        if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
        Json v;
        if (!Value(v, depth + 1)) return false;
        out.kv.emplace_back(std::move(key), std::move(v));
        Space();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        return s_[pos_++] == '}';
      }
    }
    if (c == '[') {
      ++pos_;
      out.type = Json::kArray;
      Space();
      if (pos_ < s_.size() && s_[pos_] == ']') return ++pos_, true;
      for (;;) {
        Json v;
        if (!Value(v, depth + 1)) return false;
        out.items.push_back(std::move(v));
        Space();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        return s_[pos_++] == ']';
      }
    }
    if (c == '"') {
      out.type = Json::kString;
      return String(out.str);
    }
    if (Lit("true")) return out.type = Json::kBool, out.b = true, true;
    if (Lit("false")) return out.type = Json::kBool, out.b = false, true;
    if (Lit("null")) return out.type = Json::kNull, true;
    char* end;
    out.num = std::strtod(s_.c_str() + pos_, &end);
    if (end == s_.c_str() + pos_) return false;
    out.type = Json::kNumber;
    pos_ = size_t(end - s_.c_str());
    return true;
  }
  const std::string& s_;
  size_t pos_ = 0;
};

bool IsHex64(const std::string& s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool PlainFileName(const std::string& f) {
  if (f.empty() || f.size() > 200 || f[0] == '.') return false;
  return std::all_of(f.begin(), f.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_';
  });
}

// "https://host:port" of an address, empty when it isn't an absolute http(s) one.
std::string Origin(const std::string& url) {
  size_t scheme = url.starts_with("https://") ? 8 : url.starts_with("http://") ? 7 : 0;
  if (!scheme) return {};
  size_t end = url.find_first_of("/?#", scheme);
  std::string o = url.substr(0, end);
  // A userinfo part ("host@evil") is never a site we published to.
  if (o.find('@') != std::string::npos || o.size() == scheme) return {};
  std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return o;
}

// Resolves a releases-file address ("/dl/..." or absolute) against the file's own address.
std::string Resolve(const std::string& url, const std::string& base) {
  if (url.starts_with("/") && !url.starts_with("//")) return Origin(base) + url;
  return url;
}

// The version string goes into the signed text, file names and the screen: [0-9A-Za-z.+-], at most 64 characters.
// tools/ci/update_signing.py plain_version applies the same rule before a release is signed.
bool PlainVersion(const std::string& v) {
  return !v.empty() && v.size() <= 64 && std::all_of(v.begin(), v.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '+' ||
                  c == '-';
         });
}

bool PlainKey(const std::string& k) {
  return !k.empty() && k.size() <= 32 && std::all_of(k.begin(), k.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}

}  // namespace

std::string SigningMessage(const std::string& version, std::vector<std::string> package_lines) {
  std::sort(package_lines.begin(), package_lines.end());
  std::string m = "skullburglar-update-v1\nversion=" + version + "\n";
  for (const auto& l : package_lines) m += l;
  return m;
}

namespace {

bool SignatureValid(const std::string& version, const std::vector<std::string>& lines, const std::string& sig_hex,
                    const std::string& public_key_hex) {
  uint8_t sig[64], key[32];
  if (public_key_hex.empty() || !HexToBytes(sig_hex, sig, 64) || !HexToBytes(public_key_hex, key, 32)) return false;
  const std::string msg = SigningMessage(version, lines);
  return Ed25519Verify(sig, reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), key);
}

}  // namespace

bool HasVersionNumber(const std::string& v) { return !VersionNumbers(v).empty(); }

bool IsNewerVersion(const std::string& a, const std::string& b) {
  auto x = VersionNumbers(a), y = VersionNumbers(b);
  x.resize(std::max(x.size(), y.size()));
  y.resize(x.size());
  return x > y;
}

UpdateInfo ParseReleases(const std::string& body, const std::string& base_url, const std::string& public_key_hex) {
  UpdateInfo info;
  Json doc;
  if (!Reader(body).Parse(doc) || doc.type != Json::kObject) {
    info.error = "unreadable reply";
    return info;
  }
  info.latest = doc.Str("latest");
  info.page = doc.Str("page");
  if (VersionNumbers(info.latest).empty() || !PlainVersion(info.latest)) {
    info.latest.clear();
    info.error = "no version in the reply";
    return info;
  }
  if (Origin(info.page).empty()) info.page.clear();

  const Json* versions = doc.Get("versions");
  if (!versions || versions->type != Json::kArray) return info;
  const std::string origin = Origin(base_url);
  const bool plain_http_ok = base_url.starts_with("http://");
  for (const Json& v : versions->items) {
    if (v.Str("version") != info.latest) continue;
    const Json* avail = v.Get("available");
    if (avail && avail->type == Json::kBool && !avail->b) break;
    // The flag isn't part of the signed text, so a signed install needs it explicitly true; the publisher also drops the
    // signature of an entry it marks unavailable, so a pruned release can't be replayed as the latest.
    const bool listed_available = avail && avail->type == Json::kBool && avail->b;
    const Json* dl = v.Get("downloads");
    if (!dl || dl->type != Json::kObject) break;
    // What the signature covers: the version and, per package, its key, file name, size and checksum.
    std::vector<std::string> lines;
    bool manifest_ok = true;
    for (const auto& [key, d] : dl->kv) {
      const Json* size = d.Get("size");
      const std::string file = d.Str("file"), sha = d.Str("sha256");
      const bool size_ok = size && size->type == Json::kNumber && size->num >= 1 && size->num <= 2e9 &&
                           size->num == double(uint64_t(size->num));
      if (!size_ok || !PlainKey(key) || !PlainFileName(file) || !IsHex64(sha)) {
        manifest_ok = false;  // such an entry is skipped below, and the signature can't cover it
        continue;
      }
      lines.push_back(key + " " + file + " " + std::to_string(uint64_t(size->num)) + " " + sha + "\n");
    }
    const bool signed_ok = manifest_ok && listed_available && SignatureValid(info.latest, lines, v.Str("signature"), public_key_hex);
    info.signed_ok = signed_ok;
    for (const auto& [key, d] : dl->kv) {
      Asset a;
      a.file = d.Str("file");
      a.sha256 = d.Str("sha256");
      a.url = Resolve(d.Str("url"), base_url);
      a.version = info.latest;
      a.signed_ok = signed_ok;
      const Json* size = d.Get("size");
      if (!size || size->type != Json::kNumber || !(size->num >= 1 && size->num <= 2e9)) continue;
      a.size = uint64_t(size->num);
      if (!PlainFileName(a.file) || !IsHex64(a.sha256)) continue;
      if (origin.empty() || Origin(a.url) != origin) continue;
      if (a.url.starts_with("http://") && !plain_http_ok) continue;
      info.downloads[key] = std::move(a);
    }
    break;
  }
  return info;
}

bool MacAppNeedsMove(const std::string& exe_dir) {
  return exe_dir.starts_with("/Volumes/") || exe_dir.find("/AppTranslocation/") != std::string::npos;
}

InstallKind InstallKindFor(const std::string& os, const std::string& exe_dir, const std::string& appimage,
                           bool windows_setup_install) {
  if (os == "windows") return exe_dir.empty() || !windows_setup_install ? InstallKind::kNone : InstallKind::kWindowsSetup;
  if (os == "macos") {
    std::filesystem::path d(exe_dir);
    if (d.filename() != "MacOS" || d.parent_path().filename() != "Contents") return InstallKind::kNone;
    if (d.parent_path().parent_path().extension() != ".app") return InstallKind::kNone;
    // A disk image is read-only, and an app still quarantined runs from a random read-only copy.
    if (MacAppNeedsMove(exe_dir)) return InstallKind::kNone;
    return InstallKind::kMacBundle;
  }
  // An unpacked tarball never updates itself: it has no marker of what belongs to the package, so it keeps "Download it".
  if (os == "linux") return appimage.empty() ? InstallKind::kNone : InstallKind::kLinuxAppImage;
  return InstallKind::kNone;
}

const char* AssetKey(InstallKind kind) {
  switch (kind) {
    case InstallKind::kWindowsSetup: return "windows";
    case InstallKind::kMacBundle: return "macos";
    case InstallKind::kLinuxAppImage: return "linux_appimage";
    default: return "";
  }
}

const Asset* PickAsset(const UpdateInfo& info, InstallKind kind) {
  if (kind == InstallKind::kNone) return nullptr;
  auto it = info.downloads.find(AssetKey(kind));
  return it == info.downloads.end() ? nullptr : &it->second;
}

// ---- SHA-256 (FIPS 180-4) ----
namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::Block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::Update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_ += len;
  while (len) {
    size_t n = std::min(len, sizeof(buf_) - buf_len_);
    std::memcpy(buf_ + buf_len_, p, n);
    buf_len_ += n, p += n, len -= n;
    if (buf_len_ == sizeof(buf_)) Block(buf_), buf_len_ = 0;
  }
}

std::string Sha256::Final() {
  uint64_t bits = total_ * 8;
  uint8_t pad = 0x80;
  Update(&pad, 1);
  uint8_t zero = 0;
  while (buf_len_ != 56) Update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
  Update(len, 8);
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (uint32_t v : h_)
    for (int s = 28; s >= 0; s -= 4) out += hex[(v >> s) & 0xF];
  return out;
}

std::string Sha256Hex(const std::string& data) {
  Sha256 h;
  h.Update(data.data(), data.size());
  return h.Final();
}

std::string Sha256File(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  Sha256 h;
  std::vector<char> buf(1 << 20);
  while (f.read(buf.data(), std::streamsize(buf.size())) || f.gcount() > 0) h.Update(buf.data(), size_t(f.gcount()));
  if (f.bad()) return {};
  return h.Final();
}

std::string VerifyDownload(const std::filesystem::path& path, const Asset& asset) {
  std::error_code ec;
  uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) return "the download is missing";
  if (size != asset.size)
    return "the download is " + std::to_string(size) + " bytes, expected " + std::to_string(asset.size);
  std::string sum = Sha256File(path);
  if (sum.empty()) return "can't read the download";
  if (sum != asset.sha256) return "the download's checksum doesn't match";
  return {};
}

}  // namespace bots::setup
