// The launcher's fonts as compiled into the game (launcher_style.h): assets/fonts, written out as byte arrays by CMake
// (launcher_font_data.cpp in the build tree).
#include "launcher_style.h"

extern const unsigned char kInterRegularTtf[], kInterSemiBoldTtf[], kOswaldSemiBoldTtf[];
extern const size_t kInterRegularTtfSize, kInterSemiBoldTtfSize, kOswaldSemiBoldTtfSize;

namespace bots::setup {

LauncherFontData EmbeddedLauncherFontData() {
  LauncherFontData d;
  d.inter_regular = kInterRegularTtf, d.inter_regular_size = kInterRegularTtfSize;
  d.inter_semibold = kInterSemiBoldTtf, d.inter_semibold_size = kInterSemiBoldTtfSize;
  d.oswald_semibold = kOswaldSemiBoldTtf, d.oswald_semibold_size = kOswaldSemiBoldTtfSize;
  return d;
}

}  // namespace bots::setup
