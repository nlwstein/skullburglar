// Linux: zenity if installed, else nothing (the setup window takes a typed path). See file_picker.h.
#include "file_picker.h"

#include <cstdio>
#include <string>

namespace bots::setup {

std::filesystem::path PickIsoFile() {
  FILE* p = popen(
      "zenity --file-selection --title='Choose your 50 Cent: Blood on the Sand disc image' "
      "--file-filter='*.iso' 2>/dev/null",
      "r");
  if (!p) return {};
  std::string out;
  char buf[512];
  while (fgets(buf, sizeof(buf), p)) out += buf;
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

}  // namespace bots::setup
