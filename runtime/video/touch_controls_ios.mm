// On-screen touch controls for iPadOS: a transparent UIKit overlay on top of the game view.
// Left: a floating analog stick (touch anywhere in the lower-left area) and a D-pad above it.
// Right: A/B/X/Y in the Xbox diamond. Top corners: LT/LB and RB/RT. Top centre: Back / Start.
// Fingers can slide between buttons. The overlay hides itself while a real controller is connected.
// The state is merged into player 1 like the keyboard (window_sdl.cpp GetHostKeyboard).
#include "touch_controls_ios.h"

#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    std::mutex g_touchMutex;
    Video::HostPad g_touchState;

    enum : int { kStick = -2, kNone = -1 };

    struct Button
    {
        const char* label;
        uint16_t bit;  // XInput button bit; 0 = trigger (see trigger)
        int trigger;   // 0 none, 1 LT, 2 RT
        UIColor* color;
        CGPoint center; // laid out in layoutSubviews
        CGFloat radius;
        CAShapeLayer* shape;
        CATextLayer* text;
    };
}

@interface PESTouchView : UIView
@end

@implementation PESTouchView
{
    Button _buttons[16];
    int _count;
    CFMutableDictionaryRef _touches; // UITouch* -> control index (kStick or button)
    CGPoint _stickOrigin;
    CGFloat _stickRadius;
    CGPoint _stickRest;
    CAShapeLayer* _stickBase;
    CAShapeLayer* _stickKnob;
    CGPoint _stickKnobPos;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.backgroundColor = UIColor.clearColor;
    self.multipleTouchEnabled = YES;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _touches = CFDictionaryCreateMutable(nullptr, 0, nullptr, nullptr);

    UIColor* grey = [UIColor colorWithWhite:0.85 alpha:1];
    const Button defs[] = {
        { "A", 0x1000, 0, [UIColor colorWithRed:0.30 green:0.80 blue:0.30 alpha:1] },
        { "B", 0x2000, 0, [UIColor colorWithRed:0.90 green:0.25 blue:0.25 alpha:1] },
        { "X", 0x4000, 0, [UIColor colorWithRed:0.25 green:0.50 blue:0.95 alpha:1] },
        { "Y", 0x8000, 0, [UIColor colorWithRed:0.95 green:0.80 blue:0.20 alpha:1] },
        { "LB", 0x0100, 0, grey }, { "RB", 0x0200, 0, grey },
        { "LT", 0, 1, grey }, { "RT", 0, 2, grey },
        { "BACK", 0x0020, 0, grey }, { "START", 0x0010, 0, grey },
        { "▲", 0x0001, 0, grey }, { "▼", 0x0002, 0, grey },
        { "◀", 0x0004, 0, grey }, { "▶", 0x0008, 0, grey },
        { "LS", 0x0040, 0, grey }, { "RS", 0x0080, 0, grey },
    };
    _count = int(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < _count; ++i)
    {
        _buttons[i] = defs[i];
        Button& b = _buttons[i];
        b.shape = [CAShapeLayer layer];
        b.shape.fillColor = [b.color colorWithAlphaComponent:0.25].CGColor;
        b.shape.strokeColor = [b.color colorWithAlphaComponent:0.7].CGColor;
        b.shape.lineWidth = 2;
        [self.layer addSublayer:b.shape];
        b.text = [CATextLayer layer];
        b.text.string = [NSString stringWithUTF8String:b.label];
        b.text.alignmentMode = kCAAlignmentCenter;
        b.text.foregroundColor = [UIColor colorWithWhite:1 alpha:0.85].CGColor;
        b.text.contentsScale = UIScreen.mainScreen.scale;
        [self.layer addSublayer:b.text];
    }
    _stickBase = [CAShapeLayer layer];
    _stickBase.fillColor = [UIColor colorWithWhite:1 alpha:0.10].CGColor;
    _stickBase.strokeColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor;
    _stickBase.lineWidth = 2;
    [self.layer addSublayer:_stickBase];
    _stickKnob = [CAShapeLayer layer];
    _stickKnob.fillColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
    [self.layer addSublayer:_stickKnob];
    return self;
}

- (void)dealloc
{
    CFRelease(_touches);
}

static UIBezierPath* Circle(CGPoint c, CGFloat r)
{
    return [UIBezierPath bezierPathWithArcCenter:c radius:r startAngle:0 endAngle:2 * M_PI clockwise:YES];
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    const CGRect bounds = self.bounds;
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat W = bounds.size.width, H = bounds.size.height;
    const CGFloat u = std::min(W, H) / 820.0; // designed for an 11" iPad in landscape (1180 x 820 pt)
    const CGFloat left = safe.left + 20 * u, right = W - safe.right - 20 * u;
    const CGFloat top = safe.top + 20 * u, bottom = H - safe.bottom - 20 * u;

    auto place = [&](int i, CGFloat x, CGFloat y, CGFloat r) { _buttons[i].center = CGPointMake(x, y); _buttons[i].radius = r; };
    // Face buttons (Xbox diamond).
    const CGPoint face = CGPointMake(right - 170 * u, bottom - 190 * u);
    const CGFloat fo = 88 * u, fr = 46 * u;
    place(0, face.x, face.y + fo, fr);        // A
    place(1, face.x + fo, face.y, fr);        // B
    place(2, face.x - fo, face.y, fr);        // X
    place(3, face.x, face.y - fo, fr);        // Y
    // Shoulders and triggers.
    place(6, left + 70 * u, top + 55 * u, 48 * u);   // LT
    place(4, left + 190 * u, top + 55 * u, 48 * u);  // LB
    place(7, right - 70 * u, top + 55 * u, 48 * u);  // RT
    place(5, right - 190 * u, top + 55 * u, 48 * u); // RB
    // Back / Start.
    place(8, W / 2 - 80 * u, top + 35 * u, 34 * u);
    place(9, W / 2 + 80 * u, top + 35 * u, 34 * u);
    // D-pad, above the stick area.
    const CGPoint dp = CGPointMake(left + 150 * u, bottom - 430 * u);
    const CGFloat dpo = 58 * u, dpr = 34 * u;
    place(10, dp.x, dp.y - dpo, dpr);
    place(11, dp.x, dp.y + dpo, dpr);
    place(12, dp.x - dpo, dp.y, dpr);
    place(13, dp.x + dpo, dp.y, dpr);
    // Stick clicks, next to the face buttons / stick.
    place(14, left + 330 * u, bottom - 40 * u, 28 * u);
    place(15, right - 330 * u, bottom - 40 * u, 28 * u);

    for (int i = 0; i < _count; ++i)
    {
        Button& b = _buttons[i];
        b.shape.path = Circle(b.center, b.radius).CGPath;
        const CGFloat fs = ([(NSString*)b.text.string length] > 2 ? 13 : 20) * u;
        b.text.fontSize = fs;
        b.text.frame = CGRectMake(b.center.x - b.radius, b.center.y - fs * 0.62, 2 * b.radius, fs * 1.3);
    }
    _stickRadius = 100 * u;
    _stickRest = CGPointMake(left + 150 * u, bottom - 150 * u);
    if (![self stickActive]) { _stickOrigin = _stickRest; _stickKnobPos = _stickRest; }
    [self drawStick];
}

- (BOOL)stickActive
{
    BOOL found = NO;
    CFDictionaryApplyFunction(_touches, [](const void*, const void* value, void* ctx) {
        if (value == (const void*)(intptr_t)kStick) *static_cast<BOOL*>(ctx) = YES;
    }, &found);
    return found;
}

- (void)drawStick
{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _stickBase.path = Circle(_stickOrigin, _stickRadius).CGPath;
    _stickKnob.path = Circle(_stickKnobPos, _stickRadius * 0.45).CGPath;
    [CATransaction commit];
}

- (int)hitTestButton:(CGPoint)p
{
    int best = kNone;
    CGFloat bestD = 1e9;
    for (int i = 0; i < _count; ++i)
    {
        const CGFloat d = hypot(p.x - _buttons[i].center.x, p.y - _buttons[i].center.y);
        if (d < _buttons[i].radius * 1.25 && d < bestD) { best = i; bestD = d; } // a little slack for thumbs
    }
    return best;
}

- (BOOL)inStickZone:(CGPoint)p
{
    const CGSize s = self.bounds.size;
    return p.x < s.width * 0.42 && p.y > s.height * 0.45;
}

- (void)publish
{
    Video::HostPad p;
    bool down[16] = {};
    CFIndex n = CFDictionaryGetCount(_touches);
    std::vector<const void*> vals(static_cast<size_t>(n));
    CFDictionaryGetKeysAndValues(_touches, nullptr, vals.data());
    for (const void* v : vals)
    {
        const int c = int(intptr_t(v));
        if (c >= 0 && c < _count) down[c] = true;
    }
    for (int i = 0; i < _count; ++i)
    {
        const Button& b = _buttons[i];
        if (down[i])
        {
            p.buttons |= b.bit;
            if (b.trigger == 1) p.lt = 255;
            if (b.trigger == 2) p.rt = 255;
        }
        b.shape.fillColor = [b.color colorWithAlphaComponent:down[i] ? 0.65 : 0.25].CGColor;
    }
    if ([self stickActive])
    {
        const CGFloat dx = (_stickKnobPos.x - _stickOrigin.x) / _stickRadius;
        const CGFloat dy = (_stickKnobPos.y - _stickOrigin.y) / _stickRadius;
        p.lx = int16_t(std::clamp(dx, -1.0, 1.0) * 32767);
        p.ly = int16_t(std::clamp(-dy, -1.0, 1.0) * 32767); // Xbox: +y is up
    }
    std::lock_guard lock(g_touchMutex);
    g_touchState = p;
}

- (void)moveStickTo:(CGPoint)p
{
    CGFloat dx = p.x - _stickOrigin.x, dy = p.y - _stickOrigin.y;
    const CGFloat d = hypot(dx, dy);
    if (d > _stickRadius) { dx *= _stickRadius / d; dy *= _stickRadius / d; }
    _stickKnobPos = CGPointMake(_stickOrigin.x + dx, _stickOrigin.y + dy);
    [self drawStick];
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        const CGPoint p = [t locationInView:self];
        int c = [self hitTestButton:p];
        if (c == kNone && ![self stickActive] && [self inStickZone:p])
        {
            c = kStick;
            _stickOrigin = p; // floating stick: centred where the thumb lands
            _stickKnobPos = p;
            [self drawStick];
        }
        if (c != kNone) CFDictionarySetValue(_touches, (__bridge const void*)t, (const void*)(intptr_t)c);
    }
    [self publish];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    for (UITouch* t in touches)
    {
        const void* v = nullptr;
        if (!CFDictionaryGetValueIfPresent(_touches, (__bridge const void*)t, &v)) continue;
        const CGPoint p = [t locationInView:self];
        if (int(intptr_t(v)) == kStick)
            [self moveStickTo:p];
        else
        {
            const int c = [self hitTestButton:p]; // slide between buttons
            if (c != kNone) CFDictionarySetValue(_touches, (__bridge const void*)t, (const void*)(intptr_t)c);
        }
    }
    [self publish];
}

- (void)endTouches:(NSSet<UITouch*>*)touches
{
    for (UITouch* t in touches)
    {
        const void* v = nullptr;
        if (!CFDictionaryGetValueIfPresent(_touches, (__bridge const void*)t, &v)) continue;
        CFDictionaryRemoveValue(_touches, (__bridge const void*)t);
        if (int(intptr_t(v)) == kStick)
        {
            _stickOrigin = _stickRest;
            _stickKnobPos = _stickRest;
            [self drawStick];
        }
    }
    [self publish];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self endTouches:touches]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self endTouches:touches]; }

- (void)releaseAll
{
    CFDictionaryRemoveAllValues(_touches);
    _stickOrigin = _stickRest;
    _stickKnobPos = _stickRest;
    [self drawStick];
    [self publish];
}
@end

namespace
{
    PESTouchView* g_view = nil;
    __weak UIWindow* g_touchWindow = nil;

    // SDL swaps the view controller's view (SDL_Metal_CreateView makes the Metal view the root
    // view), which drops subviews of the old one: always attach to the current root view.
    UIView* CurrentHost() { return g_touchWindow ? (g_touchWindow.rootViewController.view ?: g_touchWindow) : nil; }
}

namespace Video::Apple
{
    void InstallTouchControls(void* nativeWindow)
    {
        g_touchWindow = (__bridge UIWindow*)nativeWindow;
        UIView* host = CurrentHost();
        if (!host || g_view) return;
        g_view = [[PESTouchView alloc] initWithFrame:host.bounds];
        [host addSubview:g_view];
        std::printf("[input] touch controls on\n");
    }

    void RaiseTouchControls()
    {
        UIView* host = CurrentHost();
        if (!g_view || !host) return;
        if (g_view.superview != host)
        {
            g_view.frame = host.bounds;
            [host addSubview:g_view]; // moves it out of the old (detached) root view
            std::printf("[input] touch controls re-attached to the new root view\n");
        }
        else
            [host bringSubviewToFront:g_view];
    }

    void SetTouchControlsVisible(bool visible)
    {
        if (!g_view || g_view.hidden == !visible) return;
        g_view.hidden = !visible;
        if (!visible) [g_view releaseAll];
        std::printf("[input] touch controls %s\n", visible ? "shown" : "hidden (controller connected)");
    }

    HostPad GetTouchPad()
    {
        std::lock_guard lock(g_touchMutex);
        return g_touchState;
    }
}
