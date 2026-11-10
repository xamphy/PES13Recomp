// Native text prompt for Video::PromptText on Apple (the console's on-screen keyboard, e.g. the
// Become a Legend player name).
//   iPadOS: UIAlertController with a text field (the system keyboard appears; hardware keyboards work).
//   macOS : NSAlert with a text field, shown as a sheet on the game window.
// Must be called on the main thread; it returns at once and calls `done` on the main thread when
// the user answers. Returns false if nothing could be shown (no window / view controller).
#include "prompt_apple.h"

#include <TargetConditionals.h>
#if TARGET_OS_IOS
#   import <UIKit/UIKit.h>
#else
#   import <AppKit/AppKit.h>
#endif

#include <memory>

namespace
{
    NSString* ToNS(const std::u16string& s)
    {
        return [NSString stringWithCharacters:reinterpret_cast<const unichar*>(s.data()) length:s.size()];
    }

    std::u16string FromNS(NSString* s, uint32_t maxChars)
    {
        std::u16string out;
        const NSUInteger n = s ? s.length : 0;
        for (NSUInteger i = 0; i < n && (!maxChars || out.size() < maxChars); ++i)
            out += char16_t([s characterAtIndex:i]);
        return out;
    }
}

namespace Video::Apple
{
    bool ShowTextPrompt(void* nativeWindow, const std::u16string& title, const std::u16string& message,
        const std::u16string& text, uint32_t maxChars, std::function<void(bool, std::u16string)> done)
    {
        auto callback = std::make_shared<std::function<void(bool, std::u16string)>>(std::move(done));
        NSString* nsTitle = title.empty() ? @"Enter text" : ToNS(title);
        NSString* nsMessage = message.empty() ? nil : ToNS(message);
        NSString* nsText = ToNS(text);
#if TARGET_OS_IOS
        UIWindow* window = (__bridge UIWindow*)nativeWindow;
        UIViewController* vc = window ? window.rootViewController : nil;
        while (vc.presentedViewController) vc = vc.presentedViewController;
        if (!vc) return false;

        UIAlertController* alert = [UIAlertController alertControllerWithTitle:nsTitle message:nsMessage
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
            field.text = nsText;
            field.clearButtonMode = UITextFieldViewModeWhileEditing;
            field.autocapitalizationType = UITextAutocapitalizationTypeWords;
            field.autocorrectionType = UITextAutocorrectionTypeNo;
            field.returnKeyType = UIReturnKeyDone;
        }];
        __weak UIAlertController* weakAlert = alert;
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel
                                                handler:^(UIAlertAction*) { (*callback)(false, {}); }]];
        UIAlertAction* ok = [UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
            (*callback)(true, FromNS(weakAlert.textFields.firstObject.text, maxChars));
        }];
        [alert addAction:ok];
        alert.preferredAction = ok;
        [vc presentViewController:alert animated:YES completion:nil];
        return true;
#else
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = nsTitle;
        if (nsMessage) alert.informativeText = nsMessage;
        [alert addButtonWithTitle:@"OK"];     // NSAlertFirstButtonReturn, Return key
        [alert addButtonWithTitle:@"Cancel"]; // Esc
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        field.stringValue = nsText;
        alert.accessoryView = field;
        alert.window.initialFirstResponder = field;

        NSWindow* window = (__bridge NSWindow*)nativeWindow;
        auto finish = [callback, field, maxChars](NSModalResponse r) {
            const bool accepted = r == NSAlertFirstButtonReturn;
            (*callback)(accepted, accepted ? FromNS(field.stringValue, maxChars) : std::u16string());
        };
        if (window)
            [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse r) { finish(r); }];
        else
            finish([alert runModal]);
        return true;
#endif
    }
}
