// Windows Firewall state for LAN co-op. See firewall.h.
#include "firewall.h"

#include <winsock2.h>
#include <windows.h>
#include <netfw.h>
#include <shellapi.h>

#include <string>

#include <rex/logging.h>

namespace bots::setup {
namespace {

// CLSID of the NetFwPolicy2 coclass (netfw.h declares it without a definition in the import libraries).
constexpr CLSID kNetFwPolicy2 = {0xE2B3C97F, 0x6AE1, 0x41AC, {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};

std::wstring ExePath() {
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return n ? std::wstring(path, n) : std::wstring();
}

// The same file? Rules store the path as given (the prompt writes it lower-case, an installer may use %VARS%).
bool SamePath(const wchar_t* rule, const std::wstring& exe) {
  if (!rule || !*rule) return false;
  wchar_t expanded[MAX_PATH * 2];
  const DWORD n = ExpandEnvironmentStringsW(rule, expanded, DWORD(std::size(expanded)));
  return n && n <= std::size(expanded) && _wcsicmp(expanded, exe.c_str()) == 0;
}

SOCKET ask_sockets[2] = {INVALID_SOCKET, INVALID_SOCKET};

BOOL CALLBACK FindPrompt(HWND hwnd, LPARAM found) {
  if (!IsWindowVisible(hwnd)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return TRUE;
  wchar_t path[MAX_PATH * 2];
  DWORD n = DWORD(std::size(path));
  const bool picker = QueryFullProcessImageNameW(process, 0, path, &n) && n >= 15 &&
                      _wcsicmp(path + n - 15, L"\\PickerHost.exe") == 0;
  CloseHandle(process);
  if (!picker) return TRUE;
  *reinterpret_cast<bool*>(found) = true;
  return FALSE;
}

}  // namespace

FirewallState CheckFirewall() {
  const std::wstring exe = ExePath();
  if (exe.empty()) return FirewallState::kUnknown;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  FirewallState state = FirewallState::kUnknown;
  INetFwPolicy2* policy = nullptr;
  if (SUCCEEDED(CoCreateInstance(kNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) {
    long current = 0;
    policy->get_CurrentProfileTypes(&current);
    bool firewall_on = false;
    for (long type : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
      VARIANT_BOOL on = VARIANT_FALSE;
      if ((current & type) && SUCCEEDED(policy->get_FirewallEnabled(NET_FW_PROFILE_TYPE2(type), &on)) && on)
        firewall_on = true;
    }
    INetFwRules* rules = nullptr;
    IUnknown* unknown = nullptr;
    IEnumVARIANT* it = nullptr;
    if (!firewall_on) {
      state = FirewallState::kNotApplicable;
    } else if (SUCCEEDED(policy->get_Rules(&rules)) && SUCCEEDED(rules->get__NewEnum(&unknown)) &&
               SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&it)))) {
      bool allow = false, block = false, allow_elsewhere = false, any = false;
      VARIANT item;
      VariantInit(&item);
      while (it->Next(1, &item, nullptr) == S_OK) {
        INetFwRule* rule = nullptr;
        if (item.vt == VT_DISPATCH && item.pdispVal && SUCCEEDED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) {
          BSTR app = nullptr;
          NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
          VARIANT_BOOL enabled = VARIANT_FALSE;
          if (SUCCEEDED(rule->get_ApplicationName(&app)) && SamePath(app, exe) &&
              SUCCEEDED(rule->get_Direction(&dir)) && dir == NET_FW_RULE_DIR_IN &&
              SUCCEEDED(rule->get_Enabled(&enabled)) && enabled) {
            any = true;
            long profiles = 0;
            NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
            rule->get_Profiles(&profiles);
            rule->get_Action(&action);
            const bool here = (profiles & current) != 0;
            if (action == NET_FW_ACTION_BLOCK && here) block = true;
            if (action == NET_FW_ACTION_ALLOW) (here ? allow : allow_elsewhere) = true;
          }
          SysFreeString(app);
          rule->Release();
        }
        VariantClear(&item);
      }
      // A Block rule wins over an Allow rule in Windows Firewall.
      if (block) state = FirewallState::kBlocked;
      else if (allow) state = FirewallState::kAllowed;
      else if (allow_elsewhere && (current & NET_FW_PROFILE2_PUBLIC)) state = FirewallState::kPublicNetwork;
      else if (!any) state = FirewallState::kNotAsked;
      else state = FirewallState::kBlocked;  // rules for this program, none of them allowing it here
    }
    if (it) it->Release();
    if (unknown) unknown->Release();
    if (rules) rules->Release();
    policy->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return state;
}

void AskFirewall() {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
  // Windows asks when a program listens on a network interface. Port 0 (any free port): the rules Windows writes
  // are for the program, not a port, and the game's own ports stay free.
  const int types[2] = {SOCK_STREAM, SOCK_DGRAM};
  for (int i = 0; i < 2; ++i) {
    if (ask_sockets[i] != INVALID_SOCKET) continue;
    SOCKET s = socket(AF_INET, types[i], types[i] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == INVALID_SOCKET) continue;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        (types[i] == SOCK_STREAM && listen(s, 1) != 0)) {
      closesocket(s);
      continue;
    }
    ask_sockets[i] = s;
  }
  REXLOG_INFO("setup: opened LAN sockets so Windows Firewall asks now");
}

bool FirewallPromptOpen() {
  bool found = false;
  EnumWindows(FindPrompt, reinterpret_cast<LPARAM>(&found));
  return found;
}

void CloseFirewallAsk() {
  for (SOCKET& s : ask_sockets) {
    if (s != INVALID_SOCKET) closesocket(s), s = INVALID_SOCKET;
  }
}

bool FixFirewall() {
  const std::wstring exe = ExePath();
  if (exe.empty()) return false;
  // Remove this program's rules (a dismissed prompt leaves Block rules), then allow it on private networks.
  const std::wstring args = L"/c netsh advfirewall firewall delete rule name=all program=\"" + exe +
                            L"\" & netsh advfirewall firewall add rule name=\"Skullburglar\" dir=in action=allow "
                            L"enable=yes profile=private program=\"" + exe + L"\"";
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";
  info.lpFile = L"cmd.exe";
  info.lpParameters = args.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info) || !info.hProcess) {
    REXLOG_WARN("setup: firewall fix not run (error {})", GetLastError());
    return false;
  }
  WaitForSingleObject(info.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  REXLOG_INFO("setup: firewall fix exit code {}", code);
  return code == 0;
}

}  // namespace bots::setup
