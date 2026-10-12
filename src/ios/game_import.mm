// First-run game import for iOS and tvOS. Apple TV has no Files app, so the app serves a small web page on the local
// network: the player opens it on a phone or computer, sends their own disc image (.iso), and it is extracted with the
// desktop setup's extractor (setup/iso_extract.h) before the game starts. Uploads need the code shown on screen and are
// accepted only from private, link-local or loopback addresses.
//
// The game files land in the app's data folder (bots::ios::DataDir), which an app update keeps, so reinstalling or
// re-signing the same bundle id doesn't ask for them again. The import only runs while they are missing.

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

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/logging.h>

#if !__has_feature(objc_arc)
#error game_import.mm needs Objective-C ARC.
#endif

namespace {
namespace fs = std::filesystem;

constexpr const char* kPort = "8080";
constexpr NSUInteger kHeaderLimit = 16384;
constexpr uint64_t kImageMinimum = 1ull << 30;   // smaller than any Xbox 360 game disc
constexpr uint64_t kImageMaximum = 9ull << 30;   // a dual-layer disc is about 8.5 GB
NSString* const kStagingPrefix = @".import-";
NSString* const kStagingMarker = @"Skullburglar import v1";

enum class State { kStarting, kWaiting, kReceiving, kExtracting, kDone, kFailed };

// Shared by the server queue, the extraction thread and the import screen.
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
fs::path g_game_dir;
NSString* g_staging;
NSString* g_upload_path;
char g_code[8];
nw_listener_t g_listener;
dispatch_queue_t g_queue;
uint16_t g_port;
std::shared_ptr<bots::setup::ExtractProgress> g_progress;

// ---------- extraction

void ExtractUpload() {
  @autoreleasepool {
    g_status.Set(State::kExtracting, 0, 0, @"Checking the disc image…");
    g_progress = std::make_shared<bots::setup::ExtractProgress>();
    auto progress = g_progress;
    bool finished = false;
    std::thread watcher([progress, &finished] {
      // ExtractIso reports through atomics; copy them into the status the page and the screen read.
      while (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE)) {
        const uint64_t total = progress->bytes_total.load();
        if (total) {
          NSString* text = [NSString stringWithFormat:@"Installing %u of %u files", progress->files_done.load(),
                                                      progress->files_total.load()];
          g_status.Set(State::kExtracting, progress->bytes_done.load(), total, text);
        }
        usleep(250000);
      }
    });
    const auto result = bots::setup::ExtractIso(g_upload_path.fileSystemRepresentation, g_game_dir, *progress);
    __atomic_store_n(&finished, true, __ATOMIC_RELEASE);
    watcher.join();
    [NSFileManager.defaultManager removeItemAtPath:g_upload_path error:nil];
    if (result.ok && bots::setup::IsGameDir(g_game_dir)) {
      REXLOG_INFO("import: game files installed in {}", g_game_dir.string());
      [NSFileManager.defaultManager removeItemAtPath:g_staging error:nil];
      g_status.Set(State::kDone, 1, 1, @"Installed. Starting the game…");
    } else {
      const std::string error = result.ok ? "The disc image is missing default.xex or C1Game." : result.error;
      REXLOG_WARN("import: failed: {}", error);
      g_status.Set(State::kFailed, 0, 0, @(error.c_str()));
    }
  }
}

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
     "<h1>Skullburglar</h1><p>Send your own disc image of 50 Cent: Blood on the Sand (Xbox 360, .iso) to this device. "
     "Keep the import screen open on the device until it finishes.</p>"
     "<label for=code>Code shown on the screen</label>"
     "<input id=code type=text inputmode=numeric autocomplete=off maxlength=6>"
     "<label>Disc image</label><label id=drop for=file>Drop the disc image here, or choose it</label>"
     "<input id=file type=file accept='.iso'><progress id=bar max=1 value=0 hidden></progress><div id=status></div>"
     "<script>"
     "const $=id=>document.getElementById(id),status=t=>$('status').textContent=t;"
     "$('code').value=new URLSearchParams(location.search).get('code')||'';let busy=false;"
     "function poll(){fetch('/status').then(r=>r.json()).then(s=>{status(s.message);"
     "if(s.total){$('bar').hidden=false;$('bar').value=s.done/s.total}"
     "if(s.state=='extracting'||s.state=='receiving')setTimeout(poll,1000);else busy=false})"
     ".catch(()=>{status('Lost contact with the device.');busy=false})}"
     "function send(f){if(busy||!f)return;busy=true;const x=new XMLHttpRequest();x.open('PUT','/upload');"
     "x.setRequestHeader('X-Import-Code',$('code').value.trim());$('bar').hidden=false;$('bar').value=0;"
     "x.upload.onprogress=e=>{if(e.lengthComputable){$('bar').value=e.loaded/e.total;"
     "status('Sending '+f.name+' · '+Math.floor(100*e.loaded/e.total)+'%')}};"
     "x.onload=()=>{if(x.status==200)poll();else{status(x.responseText||'The device refused the upload.');busy=false}};"
     "x.onerror=()=>poll();x.send(f)}"
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
  static const char* names[] = {"waiting", "waiting", "receiving", "extracting", "done", "failed"};
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

void UploadFailed(BotsUploadRequest* request, NSString* message) {
  if (request.fd >= 0) {
    close(request.fd);
    request.fd = -1;
  }
  [NSFileManager.defaultManager removeItemAtPath:g_upload_path error:nil];
  REXLOG_WARN("import: upload failed: {}", message.UTF8String);
  g_status.Set(State::kFailed, 0, 0, message);
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

// False once the request has failed.
bool UploadAppend(BotsUploadRequest* request, const void* bytes, size_t size) {
  size = size_t(std::min<uint64_t>(size, request.length - request.received));
  if (!WriteAll(request.fd, bytes, size)) {
    UploadFailed(request, @"The device ran out of storage while receiving the disc image.");
    RespondText(request.connection, 507, @"The device ran out of storage.");
    return false;
  }
  request.received += size;
  g_status.Set(State::kReceiving, request.received, request.length, @"Receiving the disc image…");
  return true;
}

void UploadFinish(BotsUploadRequest* request) {
  close(request.fd);
  request.fd = -1;
  REXLOG_INFO("import: received {} bytes", request.received);
  g_status.Set(State::kExtracting, 0, 0, @"Received. Checking the disc image…");
  RespondText(request.connection, 200, @"Received.");
  std::thread(ExtractUpload).detach();
}

void ReceiveBody(BotsUploadRequest* request) {
  nw_connection_receive(request.connection, 1, 1u << 20,
                        ^(dispatch_data_t content, nw_content_context_t, bool complete, nw_error_t error) {
    __block bool ok = true;
    if (content) {
      dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* buffer, size_t size) {
        ok = UploadAppend(request, buffer, size);
        return ok;
      });
    }
    if (!ok) return;
    if (request.received == request.length) {
      UploadFinish(request);
      return;
    }
    if (error || complete) {
      UploadFailed(request, @"The upload stopped before the whole disc image arrived. Try again.");
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

void StartUpload(BotsUploadRequest* request, NSArray<NSString*>* lines, NSData* rest) {
  nw_connection_t connection = request.connection;
  NSString* code = HeaderValue(lines, @"X-Import-Code");
  NSString* expect = HeaderValue(lines, @"Expect");
  const long long length = HeaderValue(lines, @"Content-Length").longLongValue;
  const uint64_t size = length > 0 ? uint64_t(length) : 0;
  State state;
  {
    std::lock_guard lock(g_status.mutex);
    state = g_status.state;
  }
  if (!code || strcmp(code.UTF8String, g_code) != 0) {
    RespondText(connection, 403, @"That code doesn't match the one on the screen.");
    return;
  }
  if (state != State::kWaiting && state != State::kFailed) {
    RespondText(connection, 409, @"The device is already importing a disc image.");
    return;
  }
  if (size < kImageMinimum || size > kImageMaximum) {
    RespondText(connection, 413, @"That file isn't the size of an Xbox 360 disc image.");
    return;
  }
  // The upload and the files extracted from it sit side by side until the extraction finishes.
  NSNumber* available = nil;
  [[NSURL fileURLWithPath:g_staging] getResourceValue:&available forKey:NSURLVolumeAvailableCapacityKey error:nil];
  if (available && available.unsignedLongLongValue < 2 * size) {
    RespondText(connection, 507, [NSString stringWithFormat:@"The device needs %.1f GB free for this import.",
                                                            2 * size / 1e9]);
    return;
  }
  request.fd = open(g_upload_path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (request.fd < 0) {
    RespondText(connection, 507, @"Could not store the disc image on the device.");
    return;
  }
  request.length = size;
  request.received = 0;
  REXLOG_INFO("import: upload started, {} bytes", size);
  g_status.Set(State::kReceiving, 0, size, @"Receiving the disc image…");
  if (expect && [expect caseInsensitiveCompare:@"100-continue"] == NSOrderedSame) {
    static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
    dispatch_data_t data = dispatch_data_create(kContinue, sizeof(kContinue) - 1, g_queue, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_connection_send(connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true, ^(nw_error_t) {});
  }
  if (rest.length && !UploadAppend(request, rest.bytes, rest.length)) return;
  if (request.received == request.length) {
    UploadFinish(request);
    return;
  }
  ReceiveBody(request);
}

void HandleRequest(BotsUploadRequest* request, NSString* head, NSData* rest) {
  nw_connection_t connection = request.connection;
  NSArray<NSString*>* lines = [head componentsSeparatedByString:@"\r\n"];
  NSArray<NSString*>* words = [lines.firstObject componentsSeparatedByString:@" "];
  NSString* method = words.count > 1 ? words[0] : @"";
  NSString* path = words.count > 1 ? [words[1] componentsSeparatedByString:@"?"].firstObject : @"";
  if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/"]) {
    Respond(connection, 200, @"text/html; charset=utf-8", kPage);
  } else if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/status"]) {
    Respond(connection, 200, @"application/json", StatusJson());
  } else if ([method isEqualToString:@"PUT"] && [path isEqualToString:@"/upload"]) {
    StartUpload(request, lines, rest);
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

// Recover from an import cut short by a crash or by leaving the app: only folders carrying our marker are removed.
void CleanStaleStaging(NSString* root) {
  NSFileManager* files = NSFileManager.defaultManager;
  for (NSString* name in [files contentsOfDirectoryAtPath:root error:nil]) {
    if (![name hasPrefix:kStagingPrefix]) continue;
    NSString* folder = [root stringByAppendingPathComponent:name];
    NSString* owner = [NSString stringWithContentsOfFile:[folder stringByAppendingPathComponent:@"owner.txt"]
                                                encoding:NSUTF8StringEncoding
                                                   error:nil];
    if ([owner isEqualToString:kStagingMarker]) [files removeItemAtPath:folder error:nil];
  }
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
  body.text = @"To play, send your own disc image of 50 Cent: Blood on the Sand (Xbox 360, .iso)\n"
               "from a phone or computer on the same network. No game files are included.";
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
  NSFileManager* files = NSFileManager.defaultManager;
  g_game_dir = game_dir;
  NSString* root = @(game_dir.parent_path().c_str());
  [files createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
  CleanStaleStaging(root);
  g_staging = [root stringByAppendingPathComponent:[kStagingPrefix stringByAppendingString:NSUUID.UUID.UUIDString]];
  [files createDirectoryAtPath:g_staging withIntermediateDirectories:NO attributes:nil error:nil];
  [kStagingMarker writeToFile:[g_staging stringByAppendingPathComponent:@"owner.txt"]
                   atomically:YES
                     encoding:NSUTF8StringEncoding
                        error:nil];
  g_upload_path = [g_staging stringByAppendingPathComponent:@"upload.iso"];
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
