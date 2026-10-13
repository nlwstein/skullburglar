// Linux: zenity if installed, else nothing (the setup window takes a typed path). See file_picker.h.
#include "file_picker.h"

#include <cstdio>
#include <string>

namespace bots::setup {

namespace {

std::filesystem::path Zenity(const char* command) {
  FILE* p = popen(command, "r");
  if (!p) return {};
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

}  // namespace

std::filesystem::path PickIsoFile() {
  return Zenity(
      "zenity --file-selection --title='Choose your 50 Cent: Blood on the Sand disc image' "
      "--file-filter='*.iso' 2>/dev/null");
}

std::filesystem::path PickFolder() {
  return Zenity("zenity --file-selection --directory --title='Choose a folder for the game files' 2>/dev/null");
}

}  // namespace bots::setup
