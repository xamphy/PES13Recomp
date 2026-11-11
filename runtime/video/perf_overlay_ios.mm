// Performance overlay for iPadOS, drawn in the bottom letterbox bar (the 16:9 game on a 4:3 iPad
// screen leaves ~78 pt of black above and below). Two small lines, refreshed once per second:
//   58 FPS  ·  CP 9.1 / 14.0 ms  ·  3 slow  ·  1234 draws
//   RAM 1.21 GB  ·  thermal nominal  ·  battery 80% (charging)  ·  1280x720
// FPS = guest frames per second; CP = command-processor time per frame (avg / max); slow = frames
// whose CP time exceeded 16.7 ms. It sits below the touch controls and never takes touches. When
// there is no bar big enough (a 16:9 screen), it shrinks into a small pill in the top-left corner.
#include "perf_overlay_ios.h"
#include "presenter.h"
#include "touch_controls_ios.h"

#import <UIKit/UIKit.h>
#include <mach/mach.h>

#include <algorithm>
#include <cstdio>

static void ReattachIfNeeded(UIView* perfView); // below

@interface PESPerfView : UIView
@end

@implementation PESPerfView
{
    UILabel* _line1;
    UILabel* _line2;
    NSTimer* _timer;
}

- (instancetype)initWithFrame:(CGRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    self.userInteractionEnabled = NO;
    self.backgroundColor = UIColor.clearColor;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    auto make = [&](CGFloat size, UIFontWeight weight) {
        UILabel* l = [[UILabel alloc] init];
        l.font = [UIFont monospacedDigitSystemFontOfSize:size weight:weight];
        l.textColor = [UIColor colorWithWhite:1 alpha:0.75];
        l.textAlignment = NSTextAlignmentCenter;
        l.adjustsFontSizeToFitWidth = YES;
        l.minimumScaleFactor = 0.6;
        [self addSubview:l];
        return l;
    };
    _line1 = make(15, UIFontWeightSemibold);
    _line2 = make(12, UIFontWeightRegular);
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
    [self refresh];
    return self;
}

// Where the game picture ends: the same fit as the presenter's blit (RecordBlitToSwapImage).
- (CGRect)barRect
{
    const CGSize s = self.bounds.size;
    const Video::PerfStats p = Video::GetPerfStats();
    const CGFloat fw = p.frameWidth ? p.frameWidth : 16, fh = p.frameHeight ? p.frameHeight : 9;
    if (s.width * fh > s.height * fw) return CGRectZero; // pillarbox: no bottom bar
    const CGFloat pictureH = s.width * fh / fw;
    const CGFloat bar = (s.height - pictureH) / 2;
    return CGRectMake(0, s.height - bar, s.width, bar);
}

- (void)layoutSubviews
{
    [super layoutSubviews];
    const CGRect bar = [self barRect];
    const UIEdgeInsets safe = self.safeAreaInsets;
    const CGFloat usable = bar.size.height - safe.bottom;
    if (usable >= 34)
    {
        // Centre column of the bar, clear of the touch controls' stick-click buttons.
        const CGFloat w = std::min<CGFloat>(bar.size.width * 0.42, 560);
        const CGFloat x = (bar.size.width - w) / 2;
        const CGFloat y = bar.origin.y + (usable - 38) / 2;
        _line1.frame = CGRectMake(x, y, w, 20);
        _line2.frame = CGRectMake(x, y + 20, w, 16);
        _line1.backgroundColor = _line2.backgroundColor = UIColor.clearColor;
        _line2.hidden = NO;
    }
    else
    {
        // No room (16:9 screen): a compact pill in the top-left corner.
        _line1.frame = CGRectMake(safe.left + 8, safe.top + 8, 200, 22);
        _line1.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
        _line1.layer.cornerRadius = 6;
        _line1.clipsToBounds = YES;
        _line2.hidden = YES;
    }
}

static double FootprintGB()
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return double(info.phys_footprint) / (1024.0 * 1024.0 * 1024.0);
}

static const char* ThermalName(NSProcessInfoThermalState t)
{
    switch (t)
    {
    case NSProcessInfoThermalStateNominal:  return "nominal";
    case NSProcessInfoThermalStateFair:     return "fair";
    case NSProcessInfoThermalStateSerious:  return "serious";
    case NSProcessInfoThermalStateCritical: return "critical";
    }
    return "?";
}

- (void)refresh
{
    ReattachIfNeeded(self);
    const Video::PerfStats p = Video::GetPerfStats();
    UIColor* fpsColor = p.fps >= 55 ? [UIColor colorWithRed:0.45 green:0.95 blue:0.45 alpha:0.9]
                      : p.fps >= 28 ? [UIColor colorWithRed:1.0 green:0.85 blue:0.3 alpha:0.9]
                                    : [UIColor colorWithRed:1.0 green:0.4 blue:0.4 alpha:0.9];
    NSString* fps = [NSString stringWithFormat:@"%u FPS", p.fps];
    NSString* rest = [NSString stringWithFormat:@"  ·  CP %.1f / %.1f ms  ·  %u slow  ·  %u draws",
        p.cpAvgMs, p.cpMaxMs, p.slowFrames, p.drawsPerFrame];
    if (_line2.hidden) rest = @"";
    NSMutableAttributedString* s = [[NSMutableAttributedString alloc] initWithString:[fps stringByAppendingString:rest]];
    [s addAttribute:NSForegroundColorAttributeName value:fpsColor range:NSMakeRange(0, fps.length)];
    _line1.attributedText = s;

    UIDevice* dev = UIDevice.currentDevice;
    NSString* battery = @"";
    if (dev.batteryLevel >= 0)
    {
        const bool charging = dev.batteryState == UIDeviceBatteryStateCharging || dev.batteryState == UIDeviceBatteryStateFull;
        battery = [NSString stringWithFormat:@"  ·  battery %d%%%@", int(dev.batteryLevel * 100 + 0.5), charging ? @" (charging)" : @""];
    }
    _line2.text = [NSString stringWithFormat:@"RAM %.2f GB  ·  thermal %s%@  ·  %ux%u",
        FootprintGB(), ThermalName(NSProcessInfo.processInfo.thermalState), battery, p.frameWidth, p.frameHeight];
    [self setNeedsLayout]; // the game's output size can change
}
@end

namespace
{
    PESPerfView* g_perfView = nil;
    __weak UIWindow* g_perfWindow = nil;

    // SDL swaps the view controller's view (SDL_Metal_CreateView makes the Metal view the root
    // view), which drops subviews of the old one: always attach to the current root view.
    UIView* CurrentHost() { return g_perfWindow ? (g_perfWindow.rootViewController.view ?: g_perfWindow) : nil; }
}

// Safety net, once per second: if SDL replaced the root view again, re-attach both overlays
// (performance first so the touch controls stay on top).
static void ReattachIfNeeded(UIView* perfView)
{
    UIView* host = CurrentHost();
    if (host && perfView.superview != host)
    {
        Video::Apple::RaisePerfOverlay();
        Video::Apple::RaiseTouchControls();
    }
}

namespace Video::Apple
{
    void InstallPerfOverlay(void* nativeWindow)
    {
        g_perfWindow = (__bridge UIWindow*)nativeWindow;
        UIView* host = CurrentHost();
        if (!host || g_perfView) return;
        g_perfView = [[PESPerfView alloc] initWithFrame:host.bounds];
        [host addSubview:g_perfView];
        std::printf("[video] performance overlay on\n");
    }

    void RaisePerfOverlay()
    {
        UIView* host = CurrentHost();
        if (!g_perfView || !host) return;
        if (g_perfView.superview != host)
        {
            g_perfView.frame = host.bounds;
            [host addSubview:g_perfView]; // moves it out of the old (detached) root view
            std::printf("[video] performance overlay re-attached to the new root view\n");
        }
        else
            [host bringSubviewToFront:g_perfView];
    }
}
