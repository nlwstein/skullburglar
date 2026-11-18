// See config_migrate.h.
#include "config_migrate.h"

#include <fstream>
#include <system_error>

#include <rex/logging.h>

#include "../path_utf8.h"

namespace bots::setup {

int MigrateOldDefaults(const std::filesystem::path& config_path) {
  std::error_code ec;
  if (!std::filesystem::exists(config_path, ec)) return 0;
  toml::table table;
  {
    std::ifstream in(config_path, std::ios::binary);
    if (!in) return 0;
    try {
      table = toml::parse(in, bots::PathUtf8(config_path));
    } catch (const toml::parse_error&) {
      return 0;  // unreadable: the launcher reports and rewrites it
    }
  }
  const int dropped = DropOldDefaults(table, kOldDefaults, std::size(kOldDefaults));
  if (dropped == 0) return 0;
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) return 0;
  out << "# bots settings (also editable in the launcher, which opens on start)\n" << table << "\n";
  REXLOG_INFO("setup: {} saved setting(s) were an old default and are unset now, so the new default applies",
              dropped);
  return dropped;
}

}  // namespace bots::setup
