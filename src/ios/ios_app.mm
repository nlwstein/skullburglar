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

// The layout of Xbox Cloud Gaming's standard touch controls, which mirrors the controller: the left stick high on the
// left with the d-pad below and inside it; A/B/X/Y high on the right with the right stick below and inside them;
// triggers over bumpers in the top corners; View and Menu at the top centre. Monochrome: dark translucent shapes with
// a thin white outline and white glyphs. A stick starts where the thumb lands near it and returns home on release; a
// double tap holds the stick's click (L3 / R3) for as long as the thumb stays down.
namespace {
constexpr CGFloat kStickRadius = 62;  // stick travel, points
constexpr CGFloat kKnobRadius = 30;
constexpr CGFloat kFace = 56;         // face button diameter
constexpr CGFloat kHitSlop = 14;      // touch area beyond a button's edge
constexpr double kDoubleTap = 0.3;    // seconds

UIColor* Fill(bool down) { return [UIColor colorWithWhite:down ? 1 : 0 alpha:down ? 0.5 : 0.35]; }
UIColor* Outline() { return [UIColor colorWithWhite:1 alpha:0.55]; }

// A glyph as layer contents: an SF Symbol, or text in the rounded system font.
UIImage* Glyph(NSString* text, NSString* symbol, CGFloat size) {
  UIColor* color = [UIColor colorWithWhite:1 alpha:0.92];
  if (symbol) {
    UIImageSymbolConfiguration* config =
        [UIImageSymbolConfiguration configurationWithPointSize:size weight:UIImageSymbolWeightBold];
    return [[UIImage systemImageNamed:symbol withConfiguration:config]
        imageWithTintColor:color
             renderingMode:UIImageRenderingModeAlwaysOriginal];
  }
  UIFont* font = [UIFont systemFontOfSize:size weight:UIFontWeightBold];
  if (UIFontDescriptor* rounded = [font.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded]) {
    font = [UIFont fontWithDescriptor:rounded size:size];
  }
  NSDictionary* attrs = @{NSFontAttributeName : font, NSForegroundColorAttributeName : color};
  const CGSize box = [text sizeWithAttributes:attrs];
  return [[[UIGraphicsImageRenderer alloc] initWithSize:box] imageWithActions:^(UIGraphicsImageRendererContext*) {
    [text drawAtPoint:CGPointZero withAttributes:attrs];
  }];
}

CAShapeLayer* ShapeLayer(CGFloat line) {
  CAShapeLayer* layer = [CAShapeLayer layer];
  layer.fillColor = Fill(false).CGColor;
  layer.strokeColor = Outline().CGColor;
  layer.lineWidth = line;
  return layer;
}
}  // namespace

@interface BotsPadButton : NSObject
@property(nonatomic) uint16_t bit;
@property(nonatomic) uint16_t trigger;
@property(nonatomic) CGRect frame;    // laid out on every size change
@property(nonatomic) CGFloat corner;  // frame.height / 2: a circle or a pill
@property(nonatomic, strong) CALayer* root;
@property(nonatomic, strong) CAShapeLayer* shape;
@property(nonatomic, strong) CALayer* glyph;
@property(nonatomic) BOOL down;
@end
@implementation BotsPadButton
@end

// A stick with a home position. A touch inside its zone recentres it under the thumb; release sends it home.
@interface BotsPadStick : NSObject
@property(nonatomic) BOOL right;
@property(nonatomic) CGRect zone;
@property(nonatomic) CGPoint home;
@property(nonatomic) CGPoint origin;
@property(nonatomic, weak) UITouch* touch;
@property(nonatomic) BOOL click;  // double tap: the stick's click, held while this touch lasts
@property(nonatomic) CFTimeInterval lastTap;
@property(nonatomic, strong) CAShapeLayer* base;
@property(nonatomic, strong) CAShapeLayer* knob;
@property(nonatomic, strong) CALayer* label;
@end
@implementation BotsPadStick
@end

@interface BotsTouchOverlayView : UIView
@end

@implementation BotsTouchOverlayView {
  NSMutableArray<BotsPadButton*>* _buttons;
  NSMapTable<UITouch*, BotsPadButton*>* _buttonTouches;
  BotsPadStick* _sticks[2];  // left, right
  // D-pad: one control, eight directions from the angle around its centre.
  CGPoint _dpadCenter;
  CGFloat _dpadRadius;
  __weak UITouch* _dpadTouch;
  uint16_t _dpadBits;
  CAShapeLayer* _dpadBase;
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

    struct Def {
      NSString* text;
      NSString* symbol;
      uint16_t bit, trigger;
      CGFloat glyph;
    };
    // Order matters: layoutSubviews places them by index.
    const Def defs[] = {
        {@"A", nil, kA, 0, 24},  {@"B", nil, kB, 0, 24},   {@"X", nil, kX, 0, 24},   {@"Y", nil, kY, 0, 24},
        {@"LB", nil, kLB, 0, 16}, {@"RB", nil, kRB, 0, 16}, {@"LT", nil, 0, kLT, 16}, {@"RT", nil, 0, kRT, 16},
        {nil, @"rectangle.on.rectangle", kBack, 0, 14},  // View
        {nil, @"line.3.horizontal", kStart, 0, 14},       // Menu
    };
    for (const Def& d : defs) {
      BotsPadButton* b = [BotsPadButton new];
      b.bit = d.bit;
      b.trigger = d.trigger;
      b.root = [CALayer layer];
      b.shape = ShapeLayer(1.5);
      [b.root addSublayer:b.shape];
      b.glyph = [CALayer layer];
      UIImage* image = Glyph(d.text, d.symbol, d.glyph);
      b.glyph.contents = (__bridge id)image.CGImage;
      b.glyph.contentsScale = image.scale;
      b.glyph.contentsGravity = kCAGravityCenter;
      [b.root addSublayer:b.glyph];
      [self.layer addSublayer:b.root];
      [_buttons addObject:b];
    }

    _dpadBase = ShapeLayer(1.5);
    _dpadBase.fillColor = [UIColor colorWithWhite:0 alpha:0.2].CGColor;
    [self.layer addSublayer:_dpadBase];
    for (auto& arm : _dpadArms) {
      arm = ShapeLayer(1.5);
      [self.layer addSublayer:arm];
    }

    for (int i = 0; i < 2; ++i) {
      BotsPadStick* s = [BotsPadStick new];
      s.right = i == 1;
      s.base = ShapeLayer(1.5);
      s.base.fillColor = [UIColor colorWithWhite:0 alpha:0.25].CGColor;
      s.knob = ShapeLayer(1.5);
      s.knob.fillColor = [UIColor colorWithWhite:0.15 alpha:0.6].CGColor;
      s.label = [CALayer layer];
      UIImage* image = Glyph(i ? @"R" : @"L", nil, 15);
      s.label.contents = (__bridge id)image.CGImage;
      s.label.contentsScale = image.scale;
      s.label.contentsGravity = kCAGravityCenter;
      [self.layer addSublayer:s.base];
      [self.layer addSublayer:s.knob];
      [self.layer addSublayer:s.label];
      _sticks[i] = s;
    }
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  // Inside the safe area (the Dynamic Island side and the home indicator), with a small margin.
  const CGRect safe = CGRectInset(UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets), 12, 10);
  const CGFloat L = CGRectGetMinX(safe), R = CGRectGetMaxX(safe), T = CGRectGetMinY(safe), B = CGRectGetMaxY(safe);
  const CGFloat w = safe.size.width, h = safe.size.height;
  auto put = [&](NSUInteger i, CGPoint c, CGSize size) {
    BotsPadButton* b = _buttons[i];
    b.frame = CGRectMake(c.x - size.width / 2, c.y - size.height / 2, size.width, size.height);
    b.corner = std::min(size.width, size.height) / 2;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    b.root.frame = b.frame;
    b.shape.path = [UIBezierPath bezierPathWithRoundedRect:b.root.bounds cornerRadius:b.corner].CGPath;
    b.glyph.frame = b.root.bounds;
    [CATransaction commit];
  };

  // Right hand: A/B/X/Y high and outside, the right stick low and inside.
  const CGFloat gap = kFace * 1.05;
  const CGPoint face = CGPointMake(R - gap - kFace / 2, T + h * 0.52);
  put(0, CGPointMake(face.x, face.y + gap), CGSizeMake(kFace, kFace));  // A
  put(1, CGPointMake(face.x + gap, face.y), CGSizeMake(kFace, kFace));  // B
  put(2, CGPointMake(face.x - gap, face.y), CGSizeMake(kFace, kFace));  // X
  put(3, CGPointMake(face.x, face.y - gap), CGSizeMake(kFace, kFace));  // Y
  // Shoulders: the trigger in the corner, the bumper under it.
  const CGSize shoulder = CGSizeMake(88, 40);
  put(6, CGPointMake(L + shoulder.width / 2, T + shoulder.height / 2), shoulder);       // LT
  put(4, CGPointMake(L + shoulder.width / 2, T + shoulder.height * 1.7), shoulder);     // LB
  put(7, CGPointMake(R - shoulder.width / 2, T + shoulder.height / 2), shoulder);       // RT
  put(5, CGPointMake(R - shoulder.width / 2, T + shoulder.height * 1.7), shoulder);     // RB
  put(8, CGPointMake(L + w / 2 - 30, T + 20), CGSizeMake(40, 40));                       // View
  put(9, CGPointMake(L + w / 2 + 30, T + 20), CGSizeMake(40, 40));                       // Menu

  // Sticks: left high and outside, right low and inside, like the controller. Each takes touches in its quarter.
  BotsPadStick* left = _sticks[0];
  BotsPadStick* right = _sticks[1];
  left.home = CGPointMake(L + kStickRadius + 26, T + h * 0.55);
  right.home = CGPointMake(R - w * 0.30, B - kStickRadius - 4);
  const CGFloat top = T + shoulder.height * 2.3;  // below the shoulder buttons
  left.zone = CGRectMake(CGRectGetMinX(self.bounds), top, L + w * 0.30 - CGRectGetMinX(self.bounds),
                         CGRectGetMaxY(self.bounds) - top);
  right.zone = CGRectMake(L + w * 0.55, T + h * 0.45, w * 0.45 - gap * 2.2, CGRectGetMaxY(self.bounds) - T - h * 0.45);

  // D-pad: low and inside, under the left stick.
  _dpadRadius = 50;
  _dpadCenter = CGPointMake(L + w * 0.30, B - _dpadRadius - 2);
  const CGFloat arm = _dpadRadius * 0.64, thick = _dpadRadius * 0.62;
  const CGRect arms[4] = {
      CGRectMake(_dpadCenter.x - thick / 2, _dpadCenter.y - _dpadRadius, thick, arm),
      CGRectMake(_dpadCenter.x - thick / 2, _dpadCenter.y + _dpadRadius - arm, thick, arm),
      CGRectMake(_dpadCenter.x - _dpadRadius, _dpadCenter.y - thick / 2, arm, thick),
      CGRectMake(_dpadCenter.x + _dpadRadius - arm, _dpadCenter.y - thick / 2, arm, thick),
  };
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _dpadBase.path = [UIBezierPath bezierPathWithOvalInRect:CGRectInset(CGRectMake(_dpadCenter.x, _dpadCenter.y, 0, 0),
                                                                       -_dpadRadius - 6, -_dpadRadius - 6)]
                       .CGPath;
  for (int i = 0; i < 4; ++i) _dpadArms[i].path = [UIBezierPath bezierPathWithRoundedRect:arms[i] cornerRadius:6].CGPath;
  [CATransaction commit];

  for (BotsPadStick* s : {left, right}) [self drawStick:s offset:CGVectorMake(0, 0)];
  [self drawButtons];
}

- (void)drawStick:(BotsPadStick*)s offset:(CGVector)offset {
  const CGPoint c = s.touch ? s.origin : s.home;
  const CGPoint k = CGPointMake(c.x + offset.dx, c.y + offset.dy);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  s.base.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - kStickRadius, c.y - kStickRadius,
                                                                  2 * kStickRadius, 2 * kStickRadius)]
                    .CGPath;
  s.knob.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(k.x - kKnobRadius, k.y - kKnobRadius,
                                                                  2 * kKnobRadius, 2 * kKnobRadius)]
                    .CGPath;
  s.knob.fillColor = (s.click ? [UIColor colorWithWhite:1 alpha:0.55] : [UIColor colorWithWhite:0.15 alpha:0.6]).CGColor;
  s.label.frame = CGRectMake(k.x - kKnobRadius, k.y - kKnobRadius, 2 * kKnobRadius, 2 * kKnobRadius);
  const float opacity = s.touch ? 1.0 : 0.75;
  s.base.opacity = s.knob.opacity = s.label.opacity = opacity;
  [CATransaction commit];
}

- (void)drawButtons {
  [CATransaction begin];
  [CATransaction setAnimationDuration:0.05];
  for (BotsPadButton* b in _buttons) {
    b.shape.fillColor = Fill(b.down).CGColor;
    b.root.transform = b.down ? CATransform3DMakeScale(0.93, 0.93, 1) : CATransform3DIdentity;
    b.root.opacity = b.down ? 1.0 : 0.8;
  }
  const uint16_t bits[4] = {kUp, kDown, kLeft, kRight};
  for (int i = 0; i < 4; ++i) {
    const bool down = _dpadBits & bits[i];
    _dpadArms[i].fillColor = Fill(down).CGColor;
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
    const CGFloat r = b.corner;
    // Distance from p to the rounded rect: to its core segment, minus the corner radius.
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
  if (std::hypot(dx, dy) < _dpadRadius * 0.2) return 0;  // the centre is neutral
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
  if (_sticks[0].click) buttons |= kLS;
  if (_sticks[1].click) buttons |= kRS;
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

- (void)stick:(BotsPadStick*)s movedTo:(CGPoint)p {
  CGFloat dx = p.x - s.origin.x, dy = p.y - s.origin.y;
  const CGFloat len = std::hypot(dx, dy);
  if (len > kStickRadius) {
    dx *= kStickRadius / len;
    dy *= kStickRadius / len;
  }
  const auto axis = [](CGFloat v) {
    return static_cast<int16_t>(std::clamp(v / kStickRadius, CGFloat(-1), CGFloat(1)) * 32767);
  };
  auto& pad = bots::ios::TouchPad();
  (s.right ? pad.thumb_rx : pad.thumb_lx).store(axis(dx), std::memory_order_relaxed);
  (s.right ? pad.thumb_ry : pad.thumb_ly).store(axis(-dy), std::memory_order_relaxed);  // up is positive
  [self drawStick:s offset:CGVectorMake(dx, dy)];
}

- (void)releaseStick:(BotsPadStick*)s {
  s.touch = nil;
  s.click = NO;
  auto& pad = bots::ios::TouchPad();
  (s.right ? pad.thumb_rx : pad.thumb_lx).store(0, std::memory_order_relaxed);
  (s.right ? pad.thumb_ry : pad.thumb_ly).store(0, std::memory_order_relaxed);
  [self drawStick:s offset:CGVectorMake(0, 0)];
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    const CGPoint p = [t locationInView:self];
    if (!_dpadTouch && [self inDpad:p]) {
      _dpadTouch = t;
      _dpadBits = [self dpadBitsAt:p];
      continue;
    }
    if (BotsPadButton* b = [self buttonAt:p]) {
      [_buttonTouches setObject:b forKey:t];
      continue;
    }
    for (BotsPadStick* s : _sticks) {
      // The stick's own circle counts as its zone too, wherever its quarter ends.
      const bool on_stick = std::hypot(p.x - s.home.x, p.y - s.home.y) < kStickRadius + kHitSlop;
      if (s.touch || !(on_stick || CGRectContainsPoint(s.zone, p))) continue;
      s.touch = t;
      // Touching the resting stick grabs it where it is; elsewhere in the zone it recentres under the thumb.
      s.origin = on_stick ? s.home : p;
      s.click = t.timestamp - s.lastTap < kDoubleTap;
      s.lastTap = t.timestamp;
      [self stick:s movedTo:p];
      break;
    }
  }
  [self publish];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    const CGPoint p = [t locationInView:self];
    if (t == _dpadTouch) {
      _dpadBits = [self dpadBitsAt:p];
      continue;
    }
    bool stick = false;
    for (BotsPadStick* s : _sticks) {
      if (t == s.touch) {
        [self stick:s movedTo:p];
        stick = true;
      }
    }
    // Sliding onto another button presses that one instead (A -> B rolls).
    if (!stick && [_buttonTouches objectForKey:t]) {
      if (BotsPadButton* b = [self buttonAt:p]) [_buttonTouches setObject:b forKey:t];
    }
  }
  [self publish];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    for (BotsPadStick* s : _sticks) {
      if (t == s.touch) [self releaseStick:s];
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
