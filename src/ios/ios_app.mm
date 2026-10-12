// iOS and tvOS platform glue (experimental). See ios_app.h.

#include "ios/ios_app.h"

#import <GameController/GameController.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace bots::ios {

namespace {
std::filesystem::path SearchPath(NSSearchPathDirectory dir) {
  NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(dir, NSUserDomainMask, YES);
  return paths.count ? std::filesystem::path(paths.firstObject.fileSystemRepresentation)
                     : std::filesystem::path();
}
}  // namespace

std::filesystem::path DocumentsDir() { return SearchPath(NSDocumentDirectory); }
std::filesystem::path CachesDir() { return SearchPath(NSCachesDirectory); }
#if TARGET_OS_TV
std::filesystem::path DataDir() { return CachesDir(); }
#else
std::filesystem::path DataDir() { return DocumentsDir(); }
#endif

TouchPadState& TouchPad() {
  static TouchPadState state;
  return state;
}

}  // namespace bots::ios

#if !TARGET_OS_TV  // the on-screen controller (tvOS has no touch screen)
// XINPUT_GAMEPAD_* bits.
enum : uint16_t {
  kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008,
  kStart = 0x0010, kBack = 0x0020, kLS = 0x0040, kRS = 0x0080,
  kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};
enum : uint16_t { kLT = 1, kRT = 2 };  // triggers, kept apart from the button bits

// The layout follows the touch layouts players know from cloud gaming and mobile shooters: a floating move stick on
// the left, swipe-to-look on the right (finger speed drives the right stick, like a touchpad), the face diamond in the
// lower right with a large fire button above it that also aims while dragged, shoulders at the top corners, and a
// double tap on either side clicks that stick (L3 / R3) for as long as the finger stays down.
namespace {
constexpr CGFloat kStickRadius = 64;     // travel of the move stick, points
constexpr CGFloat kKnobRadius = 28;
constexpr CGFloat kFace = 60;            // face button diameter
constexpr CGFloat kHitSlop = 14;         // touch area beyond a button's edge
constexpr CGFloat kLookFullSpeed = 9;    // finger points per frame for full right-stick deflection
constexpr CGFloat kStickDeadZone = 0.24; // XInput's right-stick dead zone; small swipes start just past it
constexpr double kDoubleTap = 0.3;       // seconds

UIColor* Rgb(uint32_t rgb, CGFloat a = 1) {
  return [UIColor colorWithRed:((rgb >> 16) & 0xFF) / 255.0 green:((rgb >> 8) & 0xFF) / 255.0
                          blue:(rgb & 0xFF) / 255.0 alpha:a];
}

// A glyph as layer contents: an SF Symbol, or text in the rounded system font.
UIImage* Glyph(NSString* text, NSString* symbol, UIColor* color, CGFloat size) {
  if (symbol) {
    UIImageSymbolConfiguration* config =
        [UIImageSymbolConfiguration configurationWithPointSize:size weight:UIImageSymbolWeightBold];
    return [[UIImage systemImageNamed:symbol withConfiguration:config] imageWithTintColor:color
                                                                            renderingMode:UIImageRenderingModeAlwaysOriginal];
  }
  UIFont* font = [UIFont systemFontOfSize:size weight:UIFontWeightHeavy];
  if (UIFontDescriptor* rounded = [font.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded]) {
    font = [UIFont fontWithDescriptor:rounded size:size];
  }
  NSDictionary* attrs = @{NSFontAttributeName : font, NSForegroundColorAttributeName : color};
  const CGSize box = [text sizeWithAttributes:attrs];
  return [[[UIGraphicsImageRenderer alloc] initWithSize:box] imageWithActions:^(UIGraphicsImageRendererContext*) {
    [text drawAtPoint:CGPointZero withAttributes:attrs];
  }];
}
}  // namespace

@interface BotsPadButton : NSObject
@property(nonatomic) uint16_t bit;
@property(nonatomic) uint16_t trigger;
@property(nonatomic) BOOL aims;           // dragging from it also looks (the fire button)
@property(nonatomic) CGRect frame;        // laid out on every size change
@property(nonatomic) CGFloat corner;      // frame.height / 2 for a circle or pill
@property(nonatomic, strong) CALayer* root;
@property(nonatomic, strong) CAShapeLayer* shape;
@property(nonatomic, strong) CALayer* glyph;
@property(nonatomic) BOOL down;
@end
@implementation BotsPadButton
@end

// A floating stick: it centres where the touch lands inside its zone and rests at `home` otherwise.
@interface BotsPadStick : NSObject
@property(nonatomic) CGRect zone;
@property(nonatomic) CGPoint home;
@property(nonatomic) CGPoint origin;
@property(nonatomic, weak) UITouch* touch;
@property(nonatomic) BOOL click;          // double tap: L3 held while this touch lasts
@property(nonatomic) CFTimeInterval lastTap;
@property(nonatomic, strong) CAShapeLayer* base;
@property(nonatomic, strong) CAShapeLayer* knob;
@end
@implementation BotsPadStick
@end

@interface BotsTouchOverlayView : UIView
@end

@implementation BotsTouchOverlayView {
  NSMutableArray<BotsPadButton*>* _buttons;
  NSMapTable<UITouch*, BotsPadButton*>* _buttonTouches;
  BotsPadStick* _move;
  // Swipe-to-look: the touch, its zone, and the movement since the last display frame.
  CGRect _lookZone;
  __weak UITouch* _lookTouch;
  CGPoint _lookLast;
  CGVector _lookDelta;
  CGVector _lookSmooth;
  BOOL _lookClick;
  CFTimeInterval _lookLastTap;
  CADisplayLink* _link;
  // D-pad: one control, eight directions from the angle around its centre.
  CGPoint _dpadCenter;
  CGFloat _dpadRadius;
  __weak UITouch* _dpadTouch;
  uint16_t _dpadBits;
  CAShapeLayer* _dpadArms[4];  // up, down, left, right
  UIImpactFeedbackGenerator* _haptic;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _buttons = [NSMutableArray array];
    _buttonTouches = [NSMapTable weakToStrongObjectsMapTable];
    _haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [_haptic prepare];

    UIColor* white = [UIColor colorWithWhite:1 alpha:0.9];
    struct Def {
      NSString* text;
      NSString* symbol;
      uint32_t color;  // 0: white
      uint16_t bit, trigger;
      CGFloat glyph;
    };
    // Xbox face colours; Back and Start use the View and Menu glyphs.
    const Def defs[] = {
        {@"A", nil, 0x6DBE45, kA, 0, 26},  {@"B", nil, 0xE8483A, kB, 0, 26},
        {@"X", nil, 0x3A8FE8, kX, 0, 26},  {@"Y", nil, 0xF2C230, kY, 0, 26},
        {@"LB", nil, 0, kLB, 0, 17},       {@"RB", nil, 0, kRB, 0, 17},
        {@"LT", nil, 0, 0, kLT, 18},       {nil, @"scope", 0, 0, kRT, 30},
        {nil, @"rectangle.on.rectangle", 0, kBack, 0, 15},
        {nil, @"line.3.horizontal", 0, kStart, 0, 15},
    };
    for (const Def& d : defs) {
      BotsPadButton* b = [BotsPadButton new];
      b.bit = d.bit;
      b.trigger = d.trigger;
      b.aims = d.trigger == kRT;
      b.root = [CALayer layer];
      b.shape = [CAShapeLayer layer];
      b.shape.fillColor = [UIColor colorWithWhite:0 alpha:0.28].CGColor;
      b.shape.strokeColor = (d.color ? Rgb(d.color, 0.7) : [UIColor colorWithWhite:1 alpha:0.5]).CGColor;
      b.shape.lineWidth = d.color ? 2.5 : 1.5;
      [b.root addSublayer:b.shape];
      b.glyph = [CALayer layer];
      UIImage* image = Glyph(d.text, d.symbol, d.color ? Rgb(d.color) : white, d.glyph);
      b.glyph.contents = (__bridge id)image.CGImage;
      b.glyph.contentsScale = image.scale;
      b.glyph.contentsGravity = kCAGravityCenter;
      [b.root addSublayer:b.glyph];
      [self.layer addSublayer:b.root];
      [_buttons addObject:b];
    }

    for (auto& arm : _dpadArms) {
      arm = [CAShapeLayer layer];
      arm.fillColor = [UIColor colorWithWhite:0 alpha:0.28].CGColor;
      arm.strokeColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
      arm.lineWidth = 1.5;
      [self.layer addSublayer:arm];
    }

    _move = [BotsPadStick new];
    _move.base = [CAShapeLayer layer];
    _move.base.fillColor = [UIColor colorWithWhite:0 alpha:0.22].CGColor;
    _move.base.strokeColor = [UIColor colorWithWhite:1 alpha:0.45].CGColor;
    _move.base.lineWidth = 2;
    _move.knob = [CAShapeLayer layer];
    _move.knob.fillColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
    _move.knob.strokeColor = [UIColor colorWithWhite:1 alpha:0.6].CGColor;
    _move.knob.lineWidth = 1.5;
    [self.layer addSublayer:_move.base];
    [self.layer addSublayer:_move.knob];

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(lookTick:)];
    _link.paused = YES;
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  // Inside the safe area (the Dynamic Island side and the home indicator), with a small margin.
  const CGRect safe = CGRectInset(UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets), 10, 8);
  const CGFloat L = CGRectGetMinX(safe), R = CGRectGetMaxX(safe), T = CGRectGetMinY(safe), B = CGRectGetMaxY(safe);
  const CGFloat w = safe.size.width;
  auto put = [&](NSUInteger i, CGPoint c, CGSize size) {
    BotsPadButton* b = _buttons[i];
    b.frame = CGRectMake(c.x - size.width / 2, c.y - size.height / 2, size.width, size.height);
    b.corner = size.height / 2;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    b.root.frame = b.frame;
    b.shape.path = [UIBezierPath bezierPathWithRoundedRect:b.root.bounds cornerRadius:b.corner].CGPath;
    b.glyph.frame = b.root.bounds;
    [CATransaction commit];
  };
  const CGSize face = CGSizeMake(kFace, kFace);
  const CGFloat gap = kFace * 1.08;  // centre to centre, diamond arm
  const CGPoint diamond = CGPointMake(R - gap - kFace / 2, B - gap - kFace / 2);
  put(0, CGPointMake(diamond.x, diamond.y + gap), face);  // A
  put(1, CGPointMake(diamond.x + gap, diamond.y), face);  // B
  put(2, CGPointMake(diamond.x - gap, diamond.y), face);  // X
  put(3, CGPointMake(diamond.x, diamond.y - gap), face);  // Y
  put(4, CGPointMake(L + 62, T + 24), CGSizeMake(112, 44));  // LB
  put(5, CGPointMake(R - 62, T + 24), CGSizeMake(112, 44));  // RB
  // Fire above-left of the diamond, aim above Y: the two thumbs' resting reach on the right.
  put(7, CGPointMake(diamond.x - gap * 1.85, diamond.y - gap * 1.55), CGSizeMake(84, 84));  // RT
  put(6, CGPointMake(diamond.x + gap * 0.55, diamond.y - gap * 2.25), CGSizeMake(58, 58));  // LT
  put(8, CGPointMake(L + w / 2 - 34, T + 22), CGSizeMake(44, 44));  // Back (View)
  put(9, CGPointMake(L + w / 2 + 34, T + 22), CGSizeMake(44, 44));  // Start (Menu)

  // D-pad: right of the move stick's resting place, low enough for a thumb sliding off the stick.
  _dpadRadius = 52;
  _dpadCenter = CGPointMake(L + 250, B - 62);
  const CGFloat arm = _dpadRadius * 0.62, thick = _dpadRadius * 0.66;
  const CGRect arms[4] = {
      CGRectMake(_dpadCenter.x - thick / 2, _dpadCenter.y - _dpadRadius, thick, arm),
      CGRectMake(_dpadCenter.x - thick / 2, _dpadCenter.y + _dpadRadius - arm, thick, arm),
      CGRectMake(_dpadCenter.x - _dpadRadius, _dpadCenter.y - thick / 2, arm, thick),
      CGRectMake(_dpadCenter.x + _dpadRadius - arm, _dpadCenter.y - thick / 2, arm, thick),
  };
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  for (int i = 0; i < 4; ++i) _dpadArms[i].path = [UIBezierPath bezierPathWithRoundedRect:arms[i] cornerRadius:8].CGPath;
  [CATransaction commit];

  // Move on the left 40% below the shoulders, look on the rest; buttons take their touches first.
  _move.zone = CGRectMake(CGRectGetMinX(self.bounds), T + 52, L + w * 0.40 - CGRectGetMinX(self.bounds),
                          CGRectGetMaxY(self.bounds) - T - 52);
  _move.home = CGPointMake(L + kStickRadius + 40, B - kStickRadius - 30);
  _lookZone = CGRectMake(L + w * 0.40, T + 52, CGRectGetMaxX(self.bounds) - L - w * 0.40,
                         CGRectGetMaxY(self.bounds) - T - 52);
  [self drawMove:CGVectorMake(0, 0)];
  [self drawButtons];
}

- (void)drawMove:(CGVector)offset {
  const CGPoint c = _move.touch ? _move.origin : _move.home;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _move.base.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - kStickRadius, c.y - kStickRadius,
                                                                      2 * kStickRadius, 2 * kStickRadius)]
                        .CGPath;
  _move.knob.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x + offset.dx - kKnobRadius,
                                                                      c.y + offset.dy - kKnobRadius,
                                                                      2 * kKnobRadius, 2 * kKnobRadius)]
                        .CGPath;
  _move.base.opacity = _move.touch ? 1.0 : 0.6;
  _move.knob.opacity = _move.touch ? 1.0 : 0.6;
  _move.knob.fillColor = [UIColor colorWithWhite:1 alpha:_move.click ? 0.75 : 0.35].CGColor;
  [CATransaction commit];
}

- (void)drawButtons {
  [CATransaction begin];
  [CATransaction setAnimationDuration:0.06];
  for (BotsPadButton* b in _buttons) {
    b.shape.fillColor = [UIColor colorWithWhite:b.down ? 1 : 0 alpha:b.down ? 0.45 : 0.28].CGColor;
    b.root.transform = b.down ? CATransform3DMakeScale(0.92, 0.92, 1) : CATransform3DIdentity;
    b.root.opacity = b.down ? 1.0 : 0.8;
  }
  const uint16_t bits[4] = {kUp, kDown, kLeft, kRight};
  for (int i = 0; i < 4; ++i) {
    const bool down = _dpadBits & bits[i];
    _dpadArms[i].fillColor = [UIColor colorWithWhite:down ? 1 : 0 alpha:down ? 0.45 : 0.28].CGColor;
    _dpadArms[i].opacity = down ? 1.0 : 0.8;
  }
  [CATransaction commit];
}

// The nearest button whose shape (plus slop) holds p, so neighbours split the gap between them.
- (BotsPadButton*)buttonAt:(CGPoint)p {
  BotsPadButton* best = nil;
  CGFloat best_d = CGFLOAT_MAX;
  for (BotsPadButton* b in _buttons) {
    const CGRect f = b.frame;
    // Distance from p to the rounded rect: to its core segment, minus the corner radius.
    const CGFloat r = b.corner;
    const CGFloat cx = std::clamp(p.x, CGRectGetMinX(f) + r, CGRectGetMaxX(f) - r);
    const CGFloat cy = std::clamp(p.y, CGRectGetMinY(f) + r, CGRectGetMaxY(f) - r);
    const CGFloat d = std::hypot(p.x - cx, p.y - cy) - r;
    if (d < kHitSlop && d < best_d) {
      best = b;
      best_d = d;
    }
  }
  return best;
}

- (bool)inDpad:(CGPoint)p {
  return std::hypot(p.x - _dpadCenter.x, p.y - _dpadCenter.y) < _dpadRadius + kHitSlop;
}

- (uint16_t)dpadBitsAt:(CGPoint)p {
  const CGFloat dx = p.x - _dpadCenter.x, dy = p.y - _dpadCenter.y;
  if (std::hypot(dx, dy) < _dpadRadius * 0.18) return 0;  // the centre is neutral
  // Eight 45-degree sectors; the diagonal ones press both neighbours.
  const int sector = int(std::lround(std::atan2(-dy, dx) / (M_PI / 4))) & 7;  // 0 = right, counter-clockwise
  static constexpr uint16_t kSectors[8] = {kRight, kRight | kUp, kUp, kUp | kLeft,
                                           kLeft, kLeft | kDown, kDown, kDown | kRight};
  return kSectors[sector];
}

- (void)publish {
  auto& pad = bots::ios::TouchPad();
  uint16_t buttons = _dpadBits;
  uint8_t lt = 0, rt = 0;
  for (BotsPadButton* b in _buttons) b.down = NO;
  for (UITouch* t in _buttonTouches) {
    BotsPadButton* b = [_buttonTouches objectForKey:t];
    b.down = YES;
    buttons |= b.bit;
    if (b.trigger == kLT) lt = 255;
    if (b.trigger == kRT) rt = 255;
  }
  if (_move.click) buttons |= kLS;
  if (_lookClick) buttons |= kRS;
  // A light tap on every new press, like a controller's click.
  if ((buttons & ~pad.buttons.load(std::memory_order_relaxed)) || (lt && !pad.left_trigger.load()) ||
      (rt && !pad.right_trigger.load())) {
    [_haptic impactOccurred];
  }
  pad.buttons.store(buttons, std::memory_order_relaxed);
  pad.left_trigger.store(lt, std::memory_order_relaxed);
  pad.right_trigger.store(rt, std::memory_order_relaxed);
  [self drawButtons];
}

- (void)moveTo:(CGPoint)p {
  CGFloat dx = p.x - _move.origin.x, dy = p.y - _move.origin.y;
  const CGFloat len = std::hypot(dx, dy);
  if (len > kStickRadius) {
    // Past the rim the base follows the finger, so reversing direction is instant.
    const CGFloat over = len - kStickRadius;
    _move.origin = CGPointMake(_move.origin.x + dx / len * over, _move.origin.y + dy / len * over);
    dx *= kStickRadius / len;
    dy *= kStickRadius / len;
  }
  const auto axis = [](CGFloat v) {
    return static_cast<int16_t>(std::clamp(v / kStickRadius, CGFloat(-1), CGFloat(1)) * 32767);
  };
  auto& pad = bots::ios::TouchPad();
  pad.thumb_lx.store(axis(dx), std::memory_order_relaxed);
  pad.thumb_ly.store(axis(-dy), std::memory_order_relaxed);  // up is positive
  [self drawMove:CGVectorMake(dx, dy)];
}

// Swipe-to-look: each display frame turns the finger's movement since the last one into a right-stick deflection,
// lightly smoothed, starting just past the game's dead zone so slow drags still turn the camera.
- (void)lookTick:(CADisplayLink*)link {
  const CGFloat a = 0.55;  // smoothing: weight of the newest frame
  _lookSmooth.dx = a * _lookDelta.dx + (1 - a) * _lookSmooth.dx;
  _lookSmooth.dy = a * _lookDelta.dy + (1 - a) * _lookSmooth.dy;
  _lookDelta = CGVectorMake(0, 0);
  const auto axis = [](CGFloat v) -> int16_t {
    const CGFloat m = std::min(std::abs(v) / kLookFullSpeed, CGFloat(1));
    if (m < 0.02) return 0;
    const CGFloat out = kStickDeadZone + (1 - kStickDeadZone) * m;
    return static_cast<int16_t>(std::copysign(out, v) * 32767);
  };
  auto& pad = bots::ios::TouchPad();
  pad.thumb_rx.store(axis(_lookSmooth.dx), std::memory_order_relaxed);
  pad.thumb_ry.store(axis(-_lookSmooth.dy), std::memory_order_relaxed);
}

- (void)beginLook:(UITouch*)t at:(CGPoint)p {
  _lookTouch = t;
  _lookLast = p;
  _lookDelta = _lookSmooth = CGVectorMake(0, 0);
  _link.paused = NO;
}

- (void)endLook {
  _lookTouch = nil;
  _link.paused = YES;
  _lookDelta = _lookSmooth = CGVectorMake(0, 0);
  auto& pad = bots::ios::TouchPad();
  pad.thumb_rx.store(0, std::memory_order_relaxed);
  pad.thumb_ry.store(0, std::memory_order_relaxed);
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    const CGPoint p = [t locationInView:self];
    if (!_dpadTouch && [self inDpad:p]) {
      _dpadTouch = t;
      _dpadBits = [self dpadBitsAt:p];
    } else if (BotsPadButton* b = [self buttonAt:p]) {
      [_buttonTouches setObject:b forKey:t];
      if (b.aims && !_lookTouch) [self beginLook:t at:p];
    } else if (!_move.touch && CGRectContainsPoint(_move.zone, p)) {
      _move.touch = t;
      _move.origin = p;
      _move.click = t.timestamp - _move.lastTap < kDoubleTap;
      _move.lastTap = t.timestamp;
      [self drawMove:CGVectorMake(0, 0)];
    } else if (!_lookTouch && CGRectContainsPoint(_lookZone, p)) {
      _lookClick = t.timestamp - _lookLastTap < kDoubleTap;
      _lookLastTap = t.timestamp;
      [self beginLook:t at:p];
    }
  }
  [self publish];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    const CGPoint p = [t locationInView:self];
    if (t == _move.touch) {
      [self moveTo:p];
    } else if (t == _dpadTouch) {
      _dpadBits = [self dpadBitsAt:p];
    } else {
      if (t == _lookTouch) {
        // Coalesced touches keep fast swipes precise between frames.
        CGPoint last = _lookLast;
        for (UITouch* c in [event coalescedTouchesForTouch:t] ?: @[ t ]) {
          const CGPoint q = [c preciseLocationInView:self];
          _lookDelta.dx += q.x - last.x;
          _lookDelta.dy += q.y - last.y;
          last = q;
        }
        _lookLast = last;
      }
      BotsPadButton* held = [_buttonTouches objectForKey:t];
      // Sliding onto another button presses that one instead (A -> B rolls); the fire button holds while aiming.
      if (held && !held.aims) {
        if (BotsPadButton* b = [self buttonAt:p]) [_buttonTouches setObject:b forKey:t];
      }
    }
  }
  [self publish];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  auto& pad = bots::ios::TouchPad();
  for (UITouch* t in touches) {
    if (t == _move.touch) {
      _move.touch = nil;
      _move.click = NO;
      pad.thumb_lx.store(0, std::memory_order_relaxed);
      pad.thumb_ly.store(0, std::memory_order_relaxed);
      [self drawMove:CGVectorMake(0, 0)];
    }
    if (t == _lookTouch) {
      _lookClick = NO;
      [self endLook];
    }
    if (t == _dpadTouch) {
      _dpadTouch = nil;
      _dpadBits = 0;
    }
    [_buttonTouches removeObjectForKey:t];
  }
  [self publish];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self touchesEnded:touches withEvent:event];
}

@end
#endif  // !TARGET_OS_TV

namespace bots::ios {

namespace {
UIWindow* GameWindow() {
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:UIWindowScene.class]) continue;
    for (UIWindow* window in ((UIWindowScene*)scene).windows) {
      if (window.isKeyWindow) return window;
    }
    if (UIWindow* window = ((UIWindowScene*)scene).windows.firstObject) return window;
  }
  return nil;
}

#if !TARGET_OS_TV
void UpdateVisibility(UIView* overlay) {
  const bool controller = GCController.controllers.count > 0;
  overlay.hidden = controller;
  TouchPad().visible.store(!controller, std::memory_order_relaxed);
}
#endif

void InstallOnMain(int attempt) {
  UIWindow* window = GameWindow();
  UIView* host = window.rootViewController.view ?: window;
  if (!host) {
    // The game window may not exist yet; try again shortly.
    if (attempt < 100) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(),
                     ^{ InstallOnMain(attempt + 1); });
    }
    return;
  }
  UIApplication.sharedApplication.idleTimerDisabled = YES;
#if !TARGET_OS_TV
  BotsTouchOverlayView* overlay = [[BotsTouchOverlayView alloc] initWithFrame:host.bounds];
  [host addSubview:overlay];
  UpdateVisibility(overlay);
  for (NSNotificationName name in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification ]) {
    [NSNotificationCenter.defaultCenter addObserverForName:name
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification*) { UpdateVisibility(overlay); }];
  }
#endif
}
}  // namespace

void InstallAppUI() {
  dispatch_async(dispatch_get_main_queue(), ^{
    // iOS refuses GPU work from a backgrounded app, and the renderer can't pause yet: the next
    // submit loses the device and aborts. Quit cleanly instead (the next launch starts fresh).
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification
                                                    object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification*) { _exit(0); }];
    InstallOnMain(0);
  });
}

}  // namespace bots::ios
