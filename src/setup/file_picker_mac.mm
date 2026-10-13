// macOS: NSOpenPanel. See file_picker.h.
#include "file_picker.h"

#import <AppKit/AppKit.h>

namespace bots::setup {

std::filesystem::path PickIsoFile() {
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Choose your 50 Cent: Blood on the Sand disc image";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK && panel.URLs.count > 0) {
      return std::filesystem::path(panel.URLs[0].fileSystemRepresentation);
    }
  }
  return {};
}

std::filesystem::path PickFolder() {
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Choose a folder for the game files";
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.canCreateDirectories = YES;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK && panel.URLs.count > 0) {
      return std::filesystem::path(panel.URLs[0].fileSystemRepresentation);
    }
  }
  return {};
}

}  // namespace bots::setup
