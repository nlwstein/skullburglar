// iOS has no launcher (bots_app.h), so nothing picks a disc image or a folder there.
#include "setup/file_picker.h"

namespace bots::setup {

std::filesystem::path PickIsoFile() { return {}; }
std::filesystem::path PickFolder() { return {}; }

}  // namespace bots::setup
