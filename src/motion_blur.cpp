// Motion blur on/off (GitHub #11).
//
// The blur the player sees is not UE3's stock MotionBlurEffect: it is part of Swordfish's uber post-process
// (USfUberPostProcessEffect), whose final composite pass also does bloom, depth of field, tone mapping and gamma, and
// blurs the scene along the velocity buffer and the camera's motion. UE3's own switch (SystemSettings MotionBlur,
// bAllowMotionBlur) only turns off the stock effect and leaves this blur on, so it can't be used.
//
// sub_82D7DDD0 sets the composite's motion blur constants (the uber render, sub_82D7E850, is its only caller):
//   (this = shader parameters, r4 = view, r5 = shader, f1 = MotionBlurAmount, f2 = MaxVelocity)
// Every term of StaticVelocityParameters (camera blur) and DynamicVelocityParameters (moving objects) is scaled by
// one of the two, so with both 0 every blur sample lands on the pixel itself: the composite still runs and the
// other effects are untouched.
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(motion_blur, true, "bots/Display",
                    "The game's motion blur (the smear when the camera turns or things move fast). Off keeps every "
                    "other effect: bloom, depth of field, colour and tone mapping");

extern "C" void __imp__sub_82D7DDD0(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void sub_82D7DDD0(PPCContext& __restrict ctx, uint8_t* base) {
  if (!REXCVAR_GET(motion_blur)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("motion blur off (game amount {}, max velocity {})", ctx.f1.f64, ctx.f2.f64);
    }
    ctx.f1.f64 = 0.0;  // MotionBlurAmount
    ctx.f2.f64 = 0.0;  // MaxVelocity
  }
  __imp__sub_82D7DDD0(ctx, base);
}
