// First-run game import for iOS and tvOS. Apple TV has no Files app, so the app serves a small web page on the local
// network: the player opens it on a phone or computer and chooses their own disc image (.iso). The page reads the
// disc's file table in the browser and sends the game's files one at a time, so the device never stores the disc image
// itself and needs only the room the game takes. Files already received are skipped, so an interrupted import picks up
// where it stopped. Uploads need the code shown on screen and are accepted only from private, link-local or loopback
// addresses.
//
// The files collect in <game>.partial and become <game> once the set is complete. They live in the app's data folder
// (bots::ios::DataDir), which an app update keeps, so reinstalling or re-signing the same bundle id doesn't ask for them
// again. The import only runs while they are missing.

#include "ios/ios_app.h"
#include "setup/app_paths.h"
#include "setup/iso_extract.h"

#import <CoreImage/CoreImage.h>
#import <UIKit/UIKit.h>
#include <Network/Network.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <rex/logging.h>

#if !__has_feature(objc_arc)
#error game_import.mm needs Objective-C ARC.
#endif

namespace {
namespace fs = std::filesystem;

constexpr const char* kPort = "8080";
constexpr NSUInteger kHeaderLimit = 16384;
constexpr uint64_t kGameMaximum = 9ull << 30;  // more than a dual-layer disc holds
constexpr uint64_t kSpareSpace = 256ull << 20;

enum class State { kStarting, kWaiting, kReceiving, kDone, kFailed };

// Shared by the server queue and the import screen.
struct Status {
  std::mutex mutex;
  State state = State::kStarting;
  uint64_t done = 0, total = 0;
  std::string message = "Starting…";
  unsigned serial = 0;

  void Set(State s, uint64_t d, uint64_t t, NSString* text) {
    std::lock_guard lock(mutex);
    state = s;
    done = d;
    total = t;
    if (text) message = text.UTF8String;
    ++serial;
  }
};

Status g_status;
fs::path g_game_dir, g_partial;
char g_code[8];
nw_listener_t g_listener;
dispatch_queue_t g_queue;
uint16_t g_port;
// The import in progress, from /begin. Touched only on g_queue.
uint64_t g_total_bytes, g_total_files, g_done_bytes, g_done_files;

// ---------- HTTP

NSString* const kPage =
    @"<!doctype html><html><head><meta charset=utf-8>"
     "<meta name=viewport content='width=device-width,initial-scale=1'><title>Skullburglar</title><style>"
     ":root{color-scheme:light dark;--bg:#f3f2ef;--fg:#18171a;--muted:#5f5d66;--card:#fff;--accent:#c8962c;--line:#d8d5cf}"
     "@media (prefers-color-scheme:dark){:root{--bg:#0e0d10;--fg:#ecebee;--muted:#a19fa8;--card:#18171b;--line:#2d2b32}}"
     "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}"
     "main{max-width:560px;margin:0 auto;padding:32px 16px}h1{margin:0 0 4px;font-size:28px}p{color:var(--muted);margin:0 0 20px}"
     "label{display:block;font-weight:600;margin:16px 0 6px}input[type=text]{font:inherit;font-size:22px;letter-spacing:.2em;"
     "width:9ch;padding:6px 10px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg)}"
     "#drop{display:block;border:2px dashed var(--line);border-radius:12px;padding:32px 16px;text-align:center;"
     "background:var(--card);cursor:pointer}#drop.over{border-color:var(--accent)}#file{display:none}"
     "progress{width:100%;height:12px;margin-top:20px;accent-color:var(--accent)}"
     "#status{margin-top:8px;min-height:3em;overflow-wrap:anywhere}</style></head><body><main>"
     "<h1>Skullburglar</h1><p>Choose your own disc image of 50 Cent: Blood on the Sand (Xbox 360, .iso). This page "
     "sends the game's files from it to the device. Keep this page and the import screen open until it finishes; if "
     "it stops, choose the disc image again to carry on.</p>"
     "<label for=code>Code shown on the screen</label>"
     "<input id=code type=text inputmode=numeric autocomplete=off maxlength=6>"
     "<label>Disc image</label><label id=drop for=file>Drop the disc image here, or choose it</label>"
     "<input id=file type=file accept='.iso'><progress id=bar max=1 value=0 hidden></progress><div id=status></div>"
     "<script>"
     "const $=id=>document.getElementById(id),say=t=>$('status').textContent=t,code=()=>$('code').value.trim();"
     "$('code').value=new URLSearchParams(location.search).get('code')||'';let busy=false;"
     // Reading the disc: the XDVDFS volume descriptor at sector 32 of the game partition, then each directory's
     // binary tree of entries (left, right, sector, size, attributes, name).
     "const at=async(f,o,n)=>new DataView(await f.slice(o,o+n).arrayBuffer()),"
     "str=(v,o,n)=>{let s='';for(let i=0;i<n;i++)s+=String.fromCharCode(v.getUint8(o+i));return s};"
     "async function list(f){let base=-1,root,rootSize;"
     "for(const o of [0,0xFB20,0x20600,0x2080000,0xFD90000]){if(o+32*2048+28>f.size)continue;"
     "const v=await at(f,o+32*2048,28);if(str(v,0,20)=='MICROSOFT*XBOX*MEDIA'){base=o;root=v.getUint32(20,true);"
     "rootSize=v.getUint32(24,true);break}}"
     "if(base<0)throw new Error('That isn\\'t an Xbox 360 disc image.');const out=[];"
     "async function dir(sector,size,prefix){const b=await at(f,base+sector*2048,size);"
     "async function walk(i){const p=i*4;if(p+14>size)return;const l=b.getUint16(p,true),r=b.getUint16(p+2,true);"
     "if(l==0xFFFF)return;const s=b.getUint32(p+4,true),n=b.getUint32(p+8,true),a=b.getUint8(p+12),"
     "name=str(b,p+14,b.getUint8(p+13));if(l)await walk(l);"
     "if(a&16){if(n&&name!='$SystemUpdate')await dir(s,n,prefix+name+'/')}"
     "else out.push({path:prefix+name,offset:base+s*2048,size:n});if(r)await walk(r)}await walk(0)}"
     "await dir(root,rootSize,'');return out}"
     "function call(method,url,body,progress){return new Promise((ok,fail)=>{const x=new XMLHttpRequest();"
     "x.open(method,url);x.setRequestHeader('X-Import-Code',code());if(progress)x.upload.onprogress=e=>progress(e.loaded);"
     "x.onload=()=>x.status==200?ok(x.responseText):fail(new Error(x.responseText||'The device refused the upload.'));"
     "x.onerror=()=>fail(new Error('Lost contact with the device. Choose the disc image again to carry on.'));"
     "x.send(body)})}"
     "const mb=n=>(n/1e6).toFixed(0)+' MB';"
     "async function send(f){if(busy||!f)return;busy=true;$('bar').hidden=false;$('bar').value=0;"
     "try{say('Reading the disc image…');const files=await list(f);"
     // default.xex first, so the device can turn away a different game before the rest is sent.
     "files.sort((a,b)=>(b.path=='default.xex')-(a.path=='default.xex'));"
     "const total=files.reduce((t,e)=>t+e.size,0);"
     "await call('POST','/begin?bytes='+total+'&files='+files.length);let done=0,start=Date.now(),sent=0;"
     "for(const [i,e] of files.entries()){const q='?path='+encodeURIComponent(e.path)+'&size='+e.size;"
     "if(await call('GET','/have'+q)!='yes'){await call('PUT','/file'+q,f.slice(e.offset,e.offset+e.size),n=>{"
     "$('bar').value=(done+n)/total;const rate=(sent+n)/Math.max(1,(Date.now()-start)/1000);"
     "say('Sending file '+(i+1)+' of '+files.length+' · '+mb(done+n)+' of '+mb(total)+' · '+mb(rate)+'/s')});"
     "sent+=e.size}done+=e.size;$('bar').value=done/total}"
     "say('Finishing…');await call('POST','/finish');$('bar').value=1;say('Done. The game is starting on the device.')}"
     "catch(e){say(e.message)}busy=false}"
     "$('file').onchange=e=>send(e.target.files[0]);const d=$('drop');"
     "d.ondragover=e=>{e.preventDefault();d.classList.add('over')};d.ondragleave=()=>d.classList.remove('over');"
     "d.ondrop=e=>{e.preventDefault();d.classList.remove('over');send(e.dataTransfer.files[0])};"
     "</script></main></body></html>";
}  // namespace

@interface BotsUploadRequest : NSObject
@property(nonatomic, strong) nw_connection_t connection;
@property(nonatomic, strong) NSMutableData* header;
@property(nonatomic) int fd;
@property(nonatomic) uint64_t length;
@property(nonatomic) uint64_t received;
@property(nonatomic) std::string target;  // where the file goes once complete
@end
@implementation BotsUploadRequest
@end

namespace {

void Respond(nw_connection_t connection, int code, NSString* type, NSString* body) {
  const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden"
                     : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large"
                     : code == 507 ? "Insufficient Storage" : "Error";
  NSData* bytes = [body dataUsingEncoding:NSUTF8StringEncoding];
  NSString* head = [NSString stringWithFormat:@"HTTP/1.1 %d %s\r\nContent-Type: %@\r\nContent-Length: %lu\r\n"
                                               "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                              code, reason, type, (unsigned long)bytes.length];
  NSMutableData* all = [[head dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
  [all appendData:bytes];
  dispatch_data_t data = dispatch_data_create(all.bytes, all.length, g_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  nw_connection_send(connection, data, NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true, ^(nw_error_t) {
    nw_connection_cancel(connection);
  });
}
void RespondText(nw_connection_t connection, int code, NSString* text) {
  Respond(connection, code, @"text/plain; charset=utf-8", text);
}

NSString* StatusJson() {
  static const char* names[] = {"waiting", "waiting", "receiving", "done", "failed"};
  std::lock_guard lock(g_status.mutex);
  NSDictionary* json = @{
    @"state" : @(names[int(g_status.state)]),
    @"done" : @(g_status.done),
    @"total" : @(g_status.total),
    @"message" : @(g_status.message.c_str()),
  };
  return [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:json options:0 error:nil]
                               encoding:NSUTF8StringEncoding];
}

bool PrivateV4(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
}

// Private IPv4 ranges, link-local, loopback and IPv6 unique-local only.
bool LocalPeer(nw_connection_t connection) {
  nw_endpoint_t endpoint = nw_connection_copy_endpoint(connection);
  if (nw_endpoint_get_type(endpoint) != nw_endpoint_type_address) return false;
  const sockaddr* address = nw_endpoint_get_address(endpoint);
  if (address->sa_family == AF_INET) return PrivateV4(ntohl(((const sockaddr_in*)address)->sin_addr.s_addr));
  if (address->sa_family == AF_INET6) {
    const in6_addr* ip = &((const sockaddr_in6*)address)->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(ip)) {
      return PrivateV4(uint32_t(ip->s6_addr[12]) << 24 | uint32_t(ip->s6_addr[13]) << 16 |
                       uint32_t(ip->s6_addr[14]) << 8 | ip->s6_addr[15]);
    }
    return IN6_IS_ADDR_LOOPBACK(ip) || IN6_IS_ADDR_LINKLOCAL(ip) || (ip->s6_addr[0] & 0xFE) == 0xFC;
  }
  return false;
}

// The query parameters of a request target, percent-decoded.
NSDictionary<NSString*, NSString*>* Query(NSString* target) {
  NSURLComponents* url = [NSURLComponents componentsWithString:[@"http://device" stringByAppendingString:target]];
  NSMutableDictionary* out = [NSMutableDictionary dictionary];
  for (NSURLQueryItem* item in url.queryItems) {
    if (item.value) out[item.name] = item.value;
  }
  return out;
}

// A path from the disc, relative and inside <game>.partial: no empty, "." or ".." parts and no backslashes.
bool SafePath(NSString* path, fs::path* out) {
  if (!path.length || [path hasPrefix:@"/"] || [path containsString:@"\\"] || [path containsString:@"\0"]) return false;
  for (NSString* part in [path componentsSeparatedByString:@"/"]) {
    if (!part.length || [part isEqualToString:@"."] || [part isEqualToString:@".."]) return false;
  }
  *out = g_partial / path.fileSystemRepresentation;
  return true;
}

void ShowProgress() {
  g_status.Set(State::kReceiving, g_done_bytes, g_total_bytes,
               [NSString stringWithFormat:@"Receiving the game files: %llu of %llu",
                                          (unsigned long long)g_done_files, (unsigned long long)g_total_files]);
}

bool WriteAll(int fd, const void* bytes, size_t size) {
  const char* p = static_cast<const char*>(bytes);
  while (size) {
    const ssize_t count = write(fd, p, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    p += count;
    size -= size_t(count);
  }
  return true;
}

void FileFailed(BotsUploadRequest* request, NSString* message) {
  if (request.fd >= 0) {
    close(request.fd);
    request.fd = -1;
  }
  std::error_code ec;
  fs::remove(request.target + ".part", ec);
  REXLOG_WARN("import: {}", message.UTF8String);
  g_status.Set(State::kFailed, g_done_bytes, g_total_bytes, message);
}

// The disc's default.xex has to be this game's; checked as soon as it arrives (the page sends it first).
bool RightGame(const fs::path& xex) {
  std::ifstream in(xex, std::ios::binary);
  std::vector<uint8_t> head(0x10000);
  in.read(reinterpret_cast<char*>(head.data()), head.size());
  return bots::setup::XexTitleId(head.data(), size_t(in.gcount())) == bots::setup::kTitleId;
}

void FileFinish(BotsUploadRequest* request) {
  if (request.fd >= 0) close(request.fd);
  request.fd = -1;
  const fs::path part = request.target + ".part";
  const fs::path target = request.target;
  std::error_code ec;
  if (target == g_partial / "default.xex" && !RightGame(part)) {
    fs::remove(part, ec);
    FileFailed(request, @"This disc image is a different game. Use 50 Cent: Blood on the Sand.");
    RespondText(request.connection, 400, @"This disc image is a different game. Use 50 Cent: Blood on the Sand.");
    return;
  }
  fs::rename(part, target, ec);
  if (ec) {
    FileFailed(request, @"Could not store a game file on the device.");
    RespondText(request.connection, 507, @"Could not store a game file on the device.");
    return;
  }
  g_done_bytes += request.length;
  ++g_done_files;
  ShowProgress();
  RespondText(request.connection, 200, @"Stored.");
}

// False once the request has failed.
bool FileAppend(BotsUploadRequest* request, const void* bytes, size_t size) {
  size = size_t(std::min<uint64_t>(size, request.length - request.received));
  if (!WriteAll(request.fd, bytes, size)) {
    FileFailed(request, @"The device ran out of storage while receiving the game files.");
    RespondText(request.connection, 507, @"The device ran out of storage.");
    return false;
  }
  request.received += size;
  // The screen shows the bytes of the file in flight too; the page tracks its own progress.
  g_status.Set(State::kReceiving, g_done_bytes + request.received, g_total_bytes, nil);
  return true;
}

void ReceiveBody(BotsUploadRequest* request) {
  nw_connection_receive(request.connection, 1, 1u << 20,
                        ^(dispatch_data_t content, nw_content_context_t, bool complete, nw_error_t error) {
    __block bool ok = true;
    if (content) {
      dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* buffer, size_t size) {
        ok = FileAppend(request, buffer, size);
        return ok;
      });
    }
    if (!ok) return;
    if (request.received == request.length) {
      FileFinish(request);
      return;
    }
    if (error || complete) {
      FileFailed(request, @"The upload stopped part way. Choose the disc image on the page again to carry on.");
      nw_connection_cancel(request.connection);
      return;
    }
    ReceiveBody(request);
  });
}

NSString* HeaderValue(NSArray<NSString*>* lines, NSString* name) {
  for (NSString* line in lines) {
    const NSRange colon = [line rangeOfString:@":"];
    if (colon.location != NSNotFound &&
        [[line substringToIndex:colon.location] caseInsensitiveCompare:name] == NSOrderedSame) {
      return [[line substringFromIndex:colon.location + 1]
          stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    }
  }
  return nil;
}

uint64_t BytesIn(const fs::path& dir) {
  uint64_t bytes = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) bytes += it->file_size(ec);
  }
  return bytes;
}

// POST /begin?bytes=&files=: the size of the game, so the device can check its free space first.
void Begin(nw_connection_t connection, NSDictionary<NSString*, NSString*>* query) {
  const uint64_t bytes = strtoull(query[@"bytes"].UTF8String ?: "0", nullptr, 10);
  const uint64_t files = strtoull(query[@"files"].UTF8String ?: "0", nullptr, 10);
  if (!bytes || bytes > kGameMaximum || !files) {
    RespondText(connection, 413, @"That doesn't look like this game's disc image.");
    return;
  }
  std::error_code ec;
  fs::create_directories(g_partial, ec);
  // Files kept from an earlier, interrupted import count towards the space already taken.
  const uint64_t have = BytesIn(g_partial);
  const auto space = fs::space(g_partial, ec);
#if TARGET_OS_TV
  // tvOS frees other apps' caches as this one writes, and that purgeable space isn't in the free figure (nor is there a
  // tvOS call for it), so try anyway; running out mid-import fails the file, and the import resumes once there's room.
  if (!ec && have < bytes && space.available < bytes - have + kSpareSpace) {
    REXLOG_WARN("import: {:.1f} GB free for {:.1f} GB of game files; relying on tvOS to free cached data",
                space.available / 1e9, (bytes - have) / 1e9);
  }
#else
  if (!ec && have < bytes && space.available < bytes - have + kSpareSpace) {
    RespondText(connection, 507, [NSString stringWithFormat:@"The device needs %.1f GB free for the game files, and has "
                                                             "%.1f GB.",
                                                            (bytes - have + kSpareSpace) / 1e9, space.available / 1e9]);
    return;
  }
#endif
  g_total_bytes = bytes;
  g_total_files = files;
  g_done_bytes = g_done_files = 0;
  REXLOG_INFO("import: receiving {} files, {} bytes ({} kept from before)", files, bytes, have);
  ShowProgress();
  RespondText(connection, 200, @"Ready.");
}

// GET /have?path=&size=: whether that file is already here (from an interrupted import); counts it if so.
void Have(nw_connection_t connection, NSDictionary<NSString*, NSString*>* query) {
  fs::path path;
  const uint64_t size = strtoull(query[@"size"].UTF8String ?: "0", nullptr, 10);
  std::error_code ec;
  const bool have = SafePath(query[@"path"], &path) && fs::is_regular_file(path, ec) && fs::file_size(path, ec) == size;
  if (have) {
    g_done_bytes += size;
    ++g_done_files;
    ShowProgress();
  }
  RespondText(connection, 200, have ? @"yes" : @"no");
}

// PUT /file?path=: one game file as the body.
void StartFile(BotsUploadRequest* request, NSDictionary<NSString*, NSString*>* query, NSArray<NSString*>* lines,
               NSData* rest) {
  nw_connection_t connection = request.connection;
  fs::path path;
  if (!SafePath(query[@"path"], &path)) {
    RespondText(connection, 400, @"Bad file name.");
    return;
  }
  const long long length = HeaderValue(lines, @"Content-Length").longLongValue;
  if (length < 0 || uint64_t(length) > kGameMaximum) {
    RespondText(connection, 413, @"That file is too large.");
    return;
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  request.target = path.string();
  request.fd = open((request.target + ".part").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (request.fd < 0) {
    RespondText(connection, 507, @"Could not store a game file on the device.");
    return;
  }
  request.length = uint64_t(length);
  request.received = 0;
  NSString* expect = HeaderValue(lines, @"Expect");
  if (expect && [expect caseInsensitiveCompare:@"100-continue"] == NSOrderedSame) {
    static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
    dispatch_data_t data = dispatch_data_create(kContinue, sizeof(kContinue) - 1, g_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_connection_send(connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t) {});
  }
  if (rest.length && !FileAppend(request, rest.bytes, rest.length)) return;
  if (request.received == request.length) {
    FileFinish(request);
    return;
  }
  ReceiveBody(request);
}

// POST /finish: every file has arrived; <game>.partial becomes <game>.
void Finish(nw_connection_t connection) {
  if (!g_total_files || g_done_files != g_total_files || !bots::setup::IsGameDir(g_partial)) {
    RespondText(connection, 409, [NSString stringWithFormat:@"Only %llu of %llu game files have arrived. Choose the disc "
                                                             "image again to carry on.",
                                                            (unsigned long long)g_done_files,
                                                            (unsigned long long)g_total_files]);
    return;
  }
  std::error_code ec;
  fs::remove_all(g_game_dir, ec);  // only reached without a usable game folder, so anything there is partial
  fs::rename(g_partial, g_game_dir, ec);
  if (ec) {
    RespondText(connection, 507, @"Could not finish the import on the device.");
    return;
  }
  REXLOG_INFO("import: game files installed in {}", g_game_dir.string());
  g_status.Set(State::kDone, 1, 1, @"Installed. Starting the game…");
  RespondText(connection, 200, @"Installed.");
}

void HandleRequest(BotsUploadRequest* request, NSString* head, NSData* rest) {
  nw_connection_t connection = request.connection;
  NSArray<NSString*>* lines = [head componentsSeparatedByString:@"\r\n"];
  NSArray<NSString*>* words = [lines.firstObject componentsSeparatedByString:@" "];
  NSString* method = words.count > 1 ? words[0] : @"";
  NSString* target = words.count > 1 ? words[1] : @"";
  NSString* path = [target componentsSeparatedByString:@"?"].firstObject;
  if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/"]) {
    Respond(connection, 200, @"text/html; charset=utf-8", kPage);
    return;
  }
  if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/status"]) {
    Respond(connection, 200, @"application/json", StatusJson());
    return;
  }
  NSString* code = HeaderValue(lines, @"X-Import-Code");
  if (!code || strcmp(code.UTF8String, g_code) != 0) {
    RespondText(connection, 403, @"That code doesn't match the one on the screen.");
    return;
  }
  State state;
  {
    std::lock_guard lock(g_status.mutex);
    state = g_status.state;
  }
  if (state == State::kDone) {
    RespondText(connection, 409, @"The game is already installed.");
    return;
  }
  NSDictionary<NSString*, NSString*>* query = Query(target);
  if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/begin"]) {
    Begin(connection, query);
  } else if (!g_total_files) {
    RespondText(connection, 409, @"Reload the page and choose the disc image again.");
  } else if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/have"]) {
    Have(connection, query);
  } else if ([method isEqualToString:@"PUT"] && [path isEqualToString:@"/file"]) {
    StartFile(request, query, lines, rest);
  } else if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/finish"]) {
    Finish(connection);
  } else {
    RespondText(connection, 404, @"Not found.");
  }
}

void ReceiveHeader(BotsUploadRequest* request) {
  nw_connection_receive(request.connection, 1, kHeaderLimit,
                        ^(dispatch_data_t content, nw_content_context_t, bool complete, nw_error_t error) {
    if (content) {
      dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* buffer, size_t size) {
        [request.header appendBytes:buffer length:size];
        return true;
      });
    }
    const NSRange end = [request.header rangeOfData:[NSData dataWithBytes:"\r\n\r\n" length:4]
                                            options:0
                                              range:NSMakeRange(0, request.header.length)];
    if (end.location != NSNotFound) {
      NSString* head = [[NSString alloc] initWithData:[request.header subdataWithRange:NSMakeRange(0, end.location)]
                                             encoding:NSUTF8StringEncoding];
      NSData* rest = [request.header
          subdataWithRange:NSMakeRange(end.location + 4, request.header.length - end.location - 4)];
      request.header = nil;
      if (!head) {
        RespondText(request.connection, 400, @"Bad request.");
        return;
      }
      HandleRequest(request, head, rest);
      return;
    }
    if (request.header.length >= kHeaderLimit || error || complete) {
      nw_connection_cancel(request.connection);
      request.header = nil;
      return;
    }
    ReceiveHeader(request);
  });
}

void Accept(nw_connection_t connection) {
  nw_connection_set_queue(connection, g_queue);
  nw_connection_start(connection);
  if (!LocalPeer(connection)) {
    nw_connection_cancel(connection);
    return;
  }
  BotsUploadRequest* request = [BotsUploadRequest new];
  request.connection = connection;
  request.header = [NSMutableData new];
  request.fd = -1;
  ReceiveHeader(request);
}

void StartListener(const char* port) {
  nw_parameters_t parameters =
      nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
  nw_parameters_set_reuse_local_address(parameters, true);
  nw_listener_t created = nw_listener_create_with_port(port, parameters);
  if (!created) {
    g_status.Set(State::kFailed, 0, 0, @"Could not start the import server.");
    return;
  }
  g_listener = created;
  nw_listener_set_queue(created, g_queue);
  nw_listener_set_new_connection_handler(created, ^(nw_connection_t connection) { Accept(connection); });
  nw_listener_set_state_changed_handler(created, ^(nw_listener_state_t state, nw_error_t) {
    if (state == nw_listener_state_ready) {
      g_port = nw_listener_get_port(created);
      REXLOG_INFO("import: serving the upload page on port {}", g_port);
      g_status.Set(State::kWaiting, 0, 0, @"Waiting for the disc image…");
    } else if (state == nw_listener_state_failed && created == g_listener) {
      nw_listener_cancel(created);
      // The usual port is taken: let the system pick one.
      if (strcmp(port, "0") != 0) {
        StartListener("0");
      } else {
        g_status.Set(State::kFailed, 0, 0, @"Could not start the import server.");
      }
    }
  });
  nw_listener_start(created);
}

// ---------- import screen

NSString* LocalAddress() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list)) return nil;
  NSString *found = nil, *fallback = nil;
  for (ifaddrs* entry = list; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET) continue;
    if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK)) continue;
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((sockaddr_in*)entry->ifa_addr)->sin_addr, text, sizeof(text));
    if (!strncmp(entry->ifa_name, "en", 2)) {
      if (!found) found = @(text);
    } else if (!fallback && strncmp(entry->ifa_name, "utun", 4) != 0) {
      fallback = @(text);
    }
  }
  freeifaddrs(list);
  return found ?: fallback;
}

UIImage* QrImage(NSString* text) {
  CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
  [filter setValue:[text dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
  [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
  CIImage* image = [filter.outputImage imageByApplyingTransform:CGAffineTransformMakeScale(12, 12)];
  CGImageRef rendered = [[CIContext context] createCGImage:image fromRect:image.extent];
  UIImage* result = [UIImage imageWithCGImage:rendered];
  CGImageRelease(rendered);
  return result;
}

}  // namespace

@interface BotsImportController : UIViewController
@property(nonatomic, strong) UILabel* addressLabel;
@property(nonatomic, strong) UILabel* statusLabel;
@property(nonatomic, strong) UIImageView* qr;
@property(nonatomic, strong) UIProgressView* progress;
@property(nonatomic, copy) NSString* shownURL;
@property(nonatomic) unsigned shownSerial;
@end

@implementation BotsImportController
- (UILabel*)label:(UIFontTextStyle)style color:(UIColor*)color {
  UILabel* label = [UILabel new];
  label.font = [UIFont preferredFontForTextStyle:style];
  label.textColor = color;
  label.numberOfLines = 0;
  label.textAlignment = NSTextAlignmentCenter;
  return label;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithRed:0.05 green:0.045 blue:0.06 alpha:1];
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
#if TARGET_OS_TV
  const CGFloat qr = 300, spacing = 28, bar = 900;
#else
  const CGFloat qr = 150, spacing = 10, bar = 360;
#endif
  UIStackView* stack = [UIStackView new];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = spacing;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:stack];
  UILabel* title = [self label:UIFontTextStyleTitle1 color:UIColor.labelColor];
  title.text = @"Skullburglar";
  [stack addArrangedSubview:title];
  UILabel* body = [self label:UIFontTextStyleBody color:UIColor.secondaryLabelColor];
  body.text = @"To play, open the page below on a phone or computer on the same network and choose your own disc\n"
               "image of 50 Cent: Blood on the Sand (Xbox 360, .iso). No game files are included.";
  [stack addArrangedSubview:body];
  self.qr = [UIImageView new];
  self.qr.layer.magnificationFilter = kCAFilterNearest;
  self.qr.backgroundColor = UIColor.whiteColor;
  self.qr.contentMode = UIViewContentModeScaleAspectFit;
  [self.qr.widthAnchor constraintEqualToConstant:qr].active = YES;
  [self.qr.heightAnchor constraintEqualToConstant:qr].active = YES;
  [stack addArrangedSubview:self.qr];
  self.addressLabel = [self label:UIFontTextStyleHeadline color:UIColor.labelColor];
  [stack addArrangedSubview:self.addressLabel];
  UILabel* code = [self label:UIFontTextStyleTitle2 color:[UIColor colorWithRed:0.86 green:0.66 blue:0.25 alpha:1]];
  code.text = [NSString stringWithFormat:@"Code %s", g_code];
  [stack addArrangedSubview:code];
  self.progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
  self.progress.hidden = YES;
  [self.progress.widthAnchor constraintEqualToConstant:bar].active = YES;
  [stack addArrangedSubview:self.progress];
  self.statusLabel = [self label:UIFontTextStyleCallout color:UIColor.secondaryLabelColor];
  [stack addArrangedSubview:self.statusLabel];
  [NSLayoutConstraint activateConstraints:@[
    [stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
    [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
    [stack.widthAnchor constraintLessThanOrEqualToAnchor:self.view.widthAnchor multiplier:0.85],
  ]];
  self.shownSerial = ~0u;
}

// Called on a timer; true once the game files are installed.
- (BOOL)refresh {
  NSString* address = LocalAddress();
  NSString* url = address && g_port ? [NSString stringWithFormat:@"http://%@:%u", address, g_port] : nil;
  if (url && ![url isEqualToString:self.shownURL]) {
    self.shownURL = url;
    self.addressLabel.text = [NSString stringWithFormat:@"Scan the code, or open %@", url];
    self.qr.image = QrImage([NSString stringWithFormat:@"%@/?code=%s", url, g_code]);
  } else if (!url && !self.shownURL) {
    self.addressLabel.text = @"Connect to your network to continue.";
  }
  unsigned serial;
  State state;
  uint64_t done, total;
  NSString* message;
  {
    std::lock_guard lock(g_status.mutex);
    serial = g_status.serial;
    state = g_status.state;
    done = g_status.done;
    total = g_status.total;
    message = @(g_status.message.c_str());
  }
  if (serial != self.shownSerial) {
    self.shownSerial = serial;
    self.statusLabel.text = message;
    self.progress.hidden = !total;
    if (total) self.progress.progress = float(double(done) / total);
  }
  return state == State::kDone;
}
@end

namespace bots::ios {

void ExcludeFromBackup(const std::filesystem::path& dir) {
#if !TARGET_OS_TV
  // Several GB the player can import again: keep it out of iCloud and computer backups.
  NSURL* url = [NSURL fileURLWithPath:@(dir.c_str()) isDirectory:YES];
  [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
#else
  (void)dir;
#endif
}

void ImportGame(const std::filesystem::path& game_dir, std::function<void()> done) {
  g_game_dir = game_dir;
  g_partial = game_dir;
  g_partial += ".partial";  // kept between launches, so an interrupted import carries on
  snprintf(g_code, sizeof(g_code), "%06u", arc4random_uniform(1000000));
  g_queue = dispatch_queue_create("bots.import", DISPATCH_QUEUE_SERIAL);
  StartListener(kPort);

  UIWindowScene* scene = nil;
  for (UIScene* candidate in UIApplication.sharedApplication.connectedScenes) {
    if ([candidate isKindOfClass:UIWindowScene.class]) {
      scene = (UIWindowScene*)candidate;
      break;
    }
  }
  UIApplication.sharedApplication.idleTimerDisabled = YES;  // the screen locking would stop the upload
  BotsImportController* controller = [BotsImportController new];
  UIWindow* window = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] init];
  window.frame = scene ? scene.coordinateSpace.bounds : UIScreen.mainScreen.bounds;
  window.windowLevel = UIWindowLevelNormal + 2;  // over the game's window
  window.rootViewController = controller;
  [window makeKeyAndVisible];
  REXLOG_INFO("import: no game files in {}, waiting for a disc image", game_dir.string());

  __block NSTimer* timer = nil;
  __block int linger = 0;
  timer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer*) {
    if (![controller refresh]) return;
    // Let the browser's last status poll see "done" before the server stops.
    if (++linger < 15) return;
    [timer invalidate];
    timer = nil;
    dispatch_sync(g_queue, ^{
      nw_listener_cancel(g_listener);
      g_listener = nil;
    });
    window.hidden = YES;
    window.rootViewController = nil;
    ExcludeFromBackup(g_game_dir);
    done();
  }];
}

}  // namespace bots::ios
