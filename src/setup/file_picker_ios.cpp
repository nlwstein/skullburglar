// iOS has no launcher (bots_app.h), so nothing picks a disc image there.
#include "setup/file_picker.h"

namespace bots::setup {

std::filesystem::path PickIsoFile() { return {}; }

}  // namespace bots::setup
