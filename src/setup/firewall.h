// LAN co-op and Windows Firewall. Windows asks once per program whether it may use the network, the first time it
// opens a socket; in the game that happens mid-story, often behind a fullscreen window, and a prompt that is closed
// without an answer leaves Block rules behind, after which LAN games silently never show up (seen 2026-10-06). So
// the launcher asks instead (AskFirewall opens a socket while its window is in front), shows what the firewall says
// about this program (CheckFirewall, read from the rules, no admin rights needed) and offers to fix a Block
// (FixFirewall: one administrator prompt). Other systems have nothing to ask: kNotApplicable.
#pragma once

namespace bots::setup {

enum class FirewallState {
  kNotApplicable,  // not Windows, or the firewall is off for the current network
  kChecking,       // no answer yet
  kAllowed,        // an Allow rule for this program covers the current network
  kNotAsked,       // no rule yet: Windows will ask
  kBlocked,        // a Block rule for this program covers the current network
  kPublicNetwork,  // allowed on private networks only, and this network is set to Public
  kUnknown,        // the firewall couldn't be read
};

#ifdef _WIN32
// Reads the firewall rules for this executable. Can take a moment; run it off the UI thread.
FirewallState CheckFirewall();
// Opens a listening socket on every interface, so Windows asks now. The sockets stay open until CloseFirewallAsk.
void AskFirewall();
// Is Windows' prompt open? Windows writes Block rules as soon as it asks and turns them into Allow rules only when the
// player chooses Allow, so while it asks, CheckFirewall says kBlocked. (Windows 11 shows it from PickerHost.exe.)
bool FirewallPromptOpen();
void CloseFirewallAsk();
// Replaces this program's firewall rules with one that allows it on private networks. Shows Windows' administrator
// prompt and waits for it; false if it was refused or failed.
bool FixFirewall();
#else
inline FirewallState CheckFirewall() { return FirewallState::kNotApplicable; }
inline void AskFirewall() {}
inline bool FirewallPromptOpen() { return false; }
inline void CloseFirewallAsk() {}
inline bool FixFirewall() { return false; }
#endif

}  // namespace bots::setup
