// Launch menu for iPadOS / iOS: a full-screen UIKit page shown before the game starts, in the same
// design as the Windows menu (launcher/launcher_win32.cpp): the generated stadium background,
// the title and Play Game / Settings / About. Settings, background and About contents come from
// launcher/launcher_core.cpp. It has its own UIWindow, created before SDL's game window exists
// and removed on Play; the game thread waits in RunLauncher.
#include "launcher_ios.h"
#include "launcher/launcher_core.h"

#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_mutex;
    std::condition_variable g_started;
    bool g_done = false;
    UIWindow* g_launcherWindow = nil;

    UIColor* Accent() { return [UIColor colorWithRed:0.16 green:0.45 blue:0.95 alpha:1]; }
    UIColor* Glass() { return [UIColor colorWithRed:0.055 green:0.08 blue:0.135 alpha:0.82]; }
    UIColor* Dim() { return [UIColor colorWithRed:0.86 green:0.89 blue:0.94 alpha:0.72]; }
    NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

    std::string DeviceModel()
    {
        char machine[64] = {};
        size_t size = sizeof(machine);
        sysctlbyname("hw.machine", machine, &size, nullptr, 0);
        return std::string(UIDevice.currentDevice.model.UTF8String) + " (" + machine + "), iPadOS/iOS " + UIDevice.currentDevice.systemVersion.UTF8String;
    }
}

@interface PESLauncherController : UIViewController
@end

@implementation PESLauncherController
{
    Launcher::Settings _settings;
    Launcher::About _about;
    UIImageView* _background;
    UIView* _page;
    CGFloat _k; // 1 on iPad, smaller on short (iPhone) screens
    UISegmentedControl* _upscaleHeight;
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }

- (UILabel*)label:(NSString*)text size:(CGFloat)size weight:(UIFontWeight)weight color:(UIColor*)color
{
    UILabel* l = [UILabel new];
    l.text = text;
    l.font = [UIFont systemFontOfSize:size * _k weight:weight];
    l.textColor = color;
    l.numberOfLines = 0;
    return l;
}

- (UIButton*)menuButton:(NSString*)title icon:(NSString*)icon primary:(BOOL)primary action:(SEL)action
{
    UIButtonConfiguration* c = primary ? [UIButtonConfiguration filledButtonConfiguration] : [UIButtonConfiguration filledButtonConfiguration];
    c.baseBackgroundColor = primary ? Accent() : Glass();
    c.baseForegroundColor = UIColor.whiteColor;
    c.image = [UIImage systemImageNamed:icon withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:20 * _k weight:UIImageSymbolWeightBold]];
    c.imagePadding = 16 * _k;
    c.contentInsets = NSDirectionalEdgeInsetsMake(0, 24 * _k, 0, 24 * _k);
    c.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    NSDictionary* attrs = @{ NSFontAttributeName: [UIFont systemFontOfSize:22 * _k weight:UIFontWeightBold] };
    c.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:attrs];
    UIButton* b = [UIButton buttonWithConfiguration:c primaryAction:nil];
    b.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    b.layer.borderColor = [UIColor colorWithWhite:1 alpha:primary ? 0 : 0.18].CGColor;
    b.layer.borderWidth = 1;
    b.layer.cornerRadius = 14 * _k;
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    [b.heightAnchor constraintEqualToConstant:66 * _k].active = YES;
    [b.widthAnchor constraintEqualToConstant:380 * _k].active = YES;
    return b;
}

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.01 green:0.02 blue:0.05 alpha:1];
    _k = std::min<CGFloat>(1.0, std::max<CGFloat>(0.55, self.view.bounds.size.height / 900.0));
    _settings = Launcher::LoadSettings();
    std::string gpu;
    if (id<MTLDevice> dev = MTLCreateSystemDefaultDevice()) gpu = dev.name.UTF8String;
    _about = Launcher::GetAbout(DeviceModel(), gpu);

    _background = [[UIImageView alloc] initWithFrame:self.view.bounds];
    _background.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _background.contentMode = UIViewContentModeScaleAspectFill;
    _background.alpha = 0;
    [self.view addSubview:_background];
    CAGradientLayer* shade = [CAGradientLayer layer];
    shade.colors = @[ (id)[UIColor colorWithRed:0.01 green:0.015 blue:0.04 alpha:0.75].CGColor, (id)[UIColor colorWithWhite:0 alpha:0].CGColor ];
    shade.startPoint = CGPointMake(0, 0.5);
    shade.endPoint = CGPointMake(0.62, 0.5);
    UIView* shadeView = [[UIView alloc] initWithFrame:self.view.bounds];
    shadeView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    shadeView.userInteractionEnabled = NO;
    shade.frame = CGRectMake(0, 0, 4000, 4000);
    [shadeView.layer addSublayer:shade];
    [self.view addSubview:shadeView];

    // The background picture: rendered off the main thread, faded in.
    const CGSize size = self.view.bounds.size;
    const CGFloat scale = std::min<CGFloat>(UIScreen.mainScreen.scale, 1600.0 / std::max<CGFloat>(size.width, 1));
    const uint32_t w = uint32_t(size.width * scale), h = uint32_t(size.height * scale);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::vector<uint32_t> px = Launcher::RenderBackground(w, h);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
        // RenderBackground is RGBA in memory (R lowest byte): byte order 32Big with skip-last reads R,G,B,X.
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
        UIImage* image = img ? [UIImage imageWithCGImage:img] : nil;
        if (img) CGImageRelease(img);
        if (ctx) CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_background.image = image;
            [UIView animateWithDuration:0.6 animations:^{ self->_background.alpha = 1; }];
        });
    });
    [self showMain];
}

- (void)setPage:(UIView*)page
{
    page.translatesAutoresizingMaskIntoConstraints = NO;
    UIView* old = _page;
    _page = page;
    page.alpha = 0;
    [self.view addSubview:page];
    const UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [page.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:48 * _k],
        [page.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-48 * _k],
        [page.topAnchor constraintEqualToAnchor:safe.topAnchor constant:24 * _k],
        [page.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-20 * _k],
    ]];
    [UIView animateWithDuration:0.25 animations:^{ page.alpha = 1; old.alpha = 0; } completion:^(BOOL) { [old removeFromSuperview]; }];
}

// ------------------------------------------------------------------ main page
- (void)showMain
{
    UIView* page = [UIView new];
    UILabel* title = [self label:@"PES 2013" size:84 weight:UIFontWeightHeavy color:UIColor.whiteColor];
    UILabel* subtitle = [self label:NS(_about.subtitle) size:22 weight:UIFontWeightRegular color:Dim()];
    UIView* bar = [UIView new];
    bar.backgroundColor = Accent();
    [bar.heightAnchor constraintEqualToConstant:4].active = YES;
    [bar.widthAnchor constraintEqualToConstant:64 * _k].active = YES;
    UIStackView* head = [[UIStackView alloc] initWithArrangedSubviews:@[ title, subtitle, bar ]];
    head.axis = UILayoutConstraintAxisVertical;
    head.alignment = UIStackViewAlignmentLeading;
    head.spacing = 6 * _k;
    [head setCustomSpacing:18 * _k afterView:subtitle];

    UIStackView* buttons = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self menuButton:@"Play Game" icon:@"play.fill" primary:YES action:@selector(playTapped)],
        [self menuButton:@"Settings" icon:@"gearshape.fill" primary:NO action:@selector(showSettings)],
        [self menuButton:@"About" icon:@"info.circle.fill" primary:NO action:@selector(showAbout)] ]];
    buttons.axis = UILayoutConstraintAxisVertical;
    buttons.spacing = 16 * _k;

    const bool ok = _about.xexFound && _about.filesFound;
    UILabel* files = [self label:(ok ? @"✓  Game files found" : [@"⚠  " stringByAppendingString:NS(_about.gameFiles)])
                            size:16 weight:UIFontWeightMedium
                           color:ok ? [UIColor colorWithRed:0.47 green:0.86 blue:0.55 alpha:1] : [UIColor colorWithRed:1 green:0.75 blue:0.35 alpha:1]];
    UILabel* build = [self label:[@"Build " stringByAppendingString:NS(_about.build[0].value)] size:14 weight:UIFontWeightRegular
                           color:[UIColor colorWithWhite:0.85 alpha:0.55]];
    UIStackView* foot = [[UIStackView alloc] initWithArrangedSubviews:@[ files, build ]];
    foot.axis = UILayoutConstraintAxisVertical;
    foot.spacing = 4;

    for (UIView* v in @[ head, buttons, foot ]) { v.translatesAutoresizingMaskIntoConstraints = NO; [page addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [head.leadingAnchor constraintEqualToAnchor:page.leadingAnchor],
        [head.topAnchor constraintEqualToAnchor:page.topAnchor constant:40 * _k],
        [buttons.leadingAnchor constraintEqualToAnchor:page.leadingAnchor],
        [buttons.topAnchor constraintEqualToAnchor:head.bottomAnchor constant:44 * _k],
        [foot.leadingAnchor constraintEqualToAnchor:page.leadingAnchor],
        [foot.bottomAnchor constraintEqualToAnchor:page.bottomAnchor],
    ]];
    [self setPage:page];
}

// ------------------------------------------------------------------ sub pages
- (UIView*)subPageWithTitle:(NSString*)title content:(UIView*)content
{
    UIView* page = [UIView new];
    UILabel* t = [self label:title size:52 weight:UIFontWeightHeavy color:UIColor.whiteColor];
    UIButton* back = [self menuButton:@"Back" icon:@"chevron.left" primary:NO action:@selector(backTapped)];
    for (NSLayoutConstraint* c in back.constraints)
        if (c.firstAttribute == NSLayoutAttributeWidth) c.constant = 190 * _k;
    UIScrollView* scroll = [UIScrollView new];
    scroll.indicatorStyle = UIScrollViewIndicatorStyleWhite;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:content];
    for (UIView* v in @[ t, back, scroll ]) { v.translatesAutoresizingMaskIntoConstraints = NO; [page addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [t.leadingAnchor constraintEqualToAnchor:page.leadingAnchor],
        [t.topAnchor constraintEqualToAnchor:page.topAnchor constant:8 * _k],
        [back.trailingAnchor constraintEqualToAnchor:page.trailingAnchor],
        [back.centerYAnchor constraintEqualToAnchor:t.centerYAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:page.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:page.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:t.bottomAnchor constant:20 * _k],
        [scroll.bottomAnchor constraintEqualToAnchor:page.bottomAnchor],
        [content.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [content.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [content.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [content.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
    ]];
    return page;
}

// A settings row: title, description and a control on the right, on a glass card.
- (UIView*)rowWithTitle:(NSString*)title detail:(NSString*)detail control:(UIView*)control
{
    UIView* card = [UIView new];
    card.backgroundColor = Glass();
    card.layer.cornerRadius = 14 * _k;
    card.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.14].CGColor;
    card.layer.borderWidth = 1;
    UILabel* t = [self label:title size:20 weight:UIFontWeightBold color:UIColor.whiteColor];
    UILabel* d = [self label:detail size:14 weight:UIFontWeightRegular color:Dim()];
    UIStackView* text = [[UIStackView alloc] initWithArrangedSubviews:@[ t, d ]];
    text.axis = UILayoutConstraintAxisVertical;
    text.spacing = 4;
    for (UIView* v in @[ text, control ]) { v.translatesAutoresizingMaskIntoConstraints = NO; [card addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [text.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:22 * _k],
        [text.topAnchor constraintEqualToAnchor:card.topAnchor constant:16 * _k],
        [text.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-16 * _k],
        [text.trailingAnchor constraintLessThanOrEqualToAnchor:control.leadingAnchor constant:-20 * _k],
        [control.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-22 * _k],
        [control.centerYAnchor constraintEqualToAnchor:card.centerYAnchor],
    ]];
    [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return card;
}

- (UISegmentedControl*)segments:(NSArray<NSString*>*)items selected:(NSInteger)index action:(SEL)action
{
    UISegmentedControl* s = [[UISegmentedControl alloc] initWithItems:items];
    s.selectedSegmentIndex = index;
    s.selectedSegmentTintColor = Accent();
    s.backgroundColor = [UIColor colorWithWhite:1 alpha:0.10];
    [s setTitleTextAttributes:@{ NSForegroundColorAttributeName: UIColor.whiteColor,
                                 NSFontAttributeName: [UIFont systemFontOfSize:15 * _k weight:UIFontWeightSemibold] } forState:UIControlStateNormal];
    [s addTarget:self action:action forControlEvents:UIControlEventValueChanged];
    return s;
}

- (UILabel*)section:(NSString*)name
{
    return [self label:name.uppercaseString size:13 weight:UIFontWeightBold color:[UIColor colorWithRed:0.55 green:0.68 blue:1 alpha:0.9]];
}

- (void)showSettings
{
    NSMutableArray* upscalers = [NSMutableArray array];
    for (int i = 0; i < Launcher::kUpscalerCount; ++i) [upscalers addObject:@(Launcher::kUpscalerNames[i])];
    NSMutableArray* heights = [NSMutableArray array];
    for (int i = 0; i < Launcher::kUpscaleHeightCount; ++i) [heights addObject:@(Launcher::kUpscaleHeightNames[i])];
    UISegmentedControl* up = [self segments:upscalers selected:_settings.upscaler action:@selector(upscalerChanged:)];
    _upscaleHeight = [self segments:heights selected:_settings.upscaleHeight action:@selector(upscaleHeightChanged:)];
    _upscaleHeight.enabled = _settings.upscaler != 0;
    UISwitch* touch = [UISwitch new];
    touch.on = _settings.touchControls;
    touch.onTintColor = Accent();
    [touch addTarget:self action:@selector(touchChanged:) forControlEvents:UIControlEventValueChanged];
    UISwitch* overlay = [UISwitch new];
    overlay.on = _settings.perfOverlay;
    overlay.onTintColor = Accent();
    [overlay addTarget:self action:@selector(overlayChanged:) forControlEvents:UIControlEventValueChanged];

    UIStackView* content = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self section:@"Display"],
        [self rowWithTitle:@"Upscaling" detail:@"Sharper picture on the screen. FSR 1 is the sharpest, SGSR 1 is lighter on the GPU." control:up],
        [self rowWithTitle:@"Upscaling resolution" detail:@"Size the upscaler renders at before the picture is fitted to the screen. Lower sizes cost less GPU time." control:_upscaleHeight],
        [self section:@"Controls"],
        [self rowWithTitle:@"Touch controls" detail:@"On-screen buttons. Hidden while a controller is connected." control:touch],
        [self section:@"Performance"],
        [self rowWithTitle:@"Performance overlay" detail:@"FPS, memory, temperature and battery below the picture." control:overlay] ]];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 12 * _k;
    for (UIView* v in content.arrangedSubviews)
        if ([v isKindOfClass:UILabel.class]) [content setCustomSpacing:8 * _k afterView:v];
    [self setPage:[self subPageWithTitle:@"Settings" content:content]];
}

- (void)showAbout
{
    UIStackView* rows = [UIStackView new];
    rows.axis = UILayoutConstraintAxisVertical;
    rows.spacing = 6 * _k;
    auto addPair = [&](const std::string& label, const std::string& value, UIColor* color) {
        UILabel* l = [self label:NS(label) size:15 weight:UIFontWeightRegular color:Dim()];
        UILabel* v = [self label:NS(value) size:15 weight:UIFontWeightRegular color:color];
        [l.widthAnchor constraintEqualToConstant:300 * _k].active = YES;
        UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:@[ l, v ]];
        row.alignment = UIStackViewAlignmentFirstBaseline;
        [rows addArrangedSubview:row];
    };
    [rows addArrangedSubview:[self section:@"Build"]];
    for (const auto& l : _about.build) addPair(l.label, l.value, UIColor.whiteColor);
    const bool ok = _about.xexFound && _about.filesFound;
    addPair("Game files", (ok ? "✓  " : "✗  ") + _about.gameFiles,
        ok ? [UIColor colorWithRed:0.47 green:0.86 blue:0.55 alpha:1] : [UIColor colorWithRed:1 green:0.47 blue:0.43 alpha:1]);
    UILabel* credits = [self section:@"Credits"];
    [rows addArrangedSubview:credits];
    [rows setCustomSpacing:16 * _k afterView:rows.arrangedSubviews[rows.arrangedSubviews.count - 2]];
    for (const auto& l : _about.credits) addPair(l.label, l.value, Dim());
    UILabel* disclaimer = [self label:NS(_about.disclaimer) size:13 weight:UIFontWeightRegular color:Dim()];
    disclaimer.font = [UIFont italicSystemFontOfSize:13 * _k];
    [rows setCustomSpacing:16 * _k afterView:rows.arrangedSubviews.lastObject];
    [rows addArrangedSubview:disclaimer];

    UIView* card = [UIView new];
    card.backgroundColor = Glass();
    card.layer.cornerRadius = 16 * _k;
    card.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.14].CGColor;
    card.layer.borderWidth = 1;
    rows.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:rows];
    [NSLayoutConstraint activateConstraints:@[
        [rows.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:24 * _k],
        [rows.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-24 * _k],
        [rows.topAnchor constraintEqualToAnchor:card.topAnchor constant:20 * _k],
        [rows.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-20 * _k],
    ]];
    [self setPage:[self subPageWithTitle:@"About" content:card]];
}

// ------------------------------------------------------------------ actions
- (void)upscalerChanged:(UISegmentedControl*)s
{
    _settings.upscaler = int(s.selectedSegmentIndex);
    _upscaleHeight.enabled = _settings.upscaler != 0;
    Launcher::SaveSettings(_settings);
}
- (void)upscaleHeightChanged:(UISegmentedControl*)s { _settings.upscaleHeight = int(s.selectedSegmentIndex); Launcher::SaveSettings(_settings); }
- (void)touchChanged:(UISwitch*)s { _settings.touchControls = s.on; Launcher::SaveSettings(_settings); }
- (void)overlayChanged:(UISwitch*)s { _settings.perfOverlay = s.on; Launcher::SaveSettings(_settings); }
- (void)backTapped { [self showMain]; }

- (void)playTapped
{
    Launcher::SaveSettings(_settings);
    Launcher::SetCurrent(_settings);
    Launcher::ApplySettings(_settings);
    [UIView animateWithDuration:0.3 animations:^{ g_launcherWindow.alpha = 0; } completion:^(BOOL) {
        g_launcherWindow.hidden = YES;
        g_launcherWindow = nil;
        {
            std::lock_guard lock(g_mutex);
            g_done = true;
        }
        g_started.notify_all();
    }];
}
@end

namespace Video::Apple
{
    void RunLauncher()
    {
        SDL_RunOnMainThread([](void*)
        {
            UIWindowScene* scene = nil;
            for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
                if ([s isKindOfClass:UIWindowScene.class]) { scene = (UIWindowScene*)s; break; }
            g_launcherWindow = scene ? [[UIWindow alloc] initWithWindowScene:scene]
                                     : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
            g_launcherWindow.rootViewController = [PESLauncherController new];
            g_launcherWindow.windowLevel = UIWindowLevelNormal + 1;
            [g_launcherWindow makeKeyAndVisible];
        }, nullptr, false);
        std::unique_lock lock(g_mutex);
        g_started.wait(lock, [] { return g_done; });
    }
}
