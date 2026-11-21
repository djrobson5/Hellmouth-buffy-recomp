// The first start's progress window (app::PrepareGameCode, first_start.cpp): a small window with a heading, the step the build is at, a
// bar, the percentage and the time left, as the Windows one is laid out. Main thread only, once SDL has started the application (SDL makes
// the NSApplication and its menus; this only adds a window to it). No buttons: closing it asks whether to stop. Nothing here is reached by
// --prepare, --no-dialogs or a level restart.
#import <AppKit/AppKit.h>

#include <atomic>
#include <string>

#include "core/log.h"
#include "host.h"

// the host is built without ARC (sdk/harness/build.sh): what a global keeps past the autorelease pool is retained by hand
#if __has_feature(objc_arc)
#define KEEP(x) (x)
#else
#define KEEP(x) [(x) retain]
#endif

@interface BuffyFirstStartDelegate : NSObject <NSWindowDelegate>
@end

namespace {
std::atomic<bool> g_close_requested{false};
NSWindow* g_window = nil;
NSProgressIndicator* g_bar = nil;
NSTextField *g_heading = nil, *g_stage = nil, *g_percent = nil, *g_left = nil;
BuffyFirstStartDelegate* g_delegate = nil;
id g_activity = nil;  // no sleep while it works (Windows: SetThreadExecutionState)

NSString* Text(const std::string& utf8) { return [NSString stringWithUTF8String:utf8.c_str()] ?: @""; }

NSTextField* Label(NSRect frame, NSTextAlignment alignment, NSColor* color) {
    NSTextField* t = [[NSTextField alloc] initWithFrame:frame];
    t.editable = NO;
    t.selectable = NO;
    t.bezeled = NO;
    t.drawsBackground = NO;
    t.alignment = alignment;
    t.textColor = color;
    t.font = [NSFont systemFontOfSize:13];
    t.lineBreakMode = NSLineBreakByTruncatingTail;
    return t;
}
}  // namespace

@implementation BuffyFirstStartDelegate
- (BOOL)windowShouldClose:(id)sender {
    (void)sender;
    g_close_requested = true;
    return NO;  // the window goes when the build has ended, or after the player has said Stop
}
@end

namespace host {

bool ProgressOpen(const std::string& title) {
    @autoreleasepool {
        if (g_window) return true;
        if (!NSApp) return false;  // SDL has not started the application
        const NSRect content = NSMakeRect(0, 0, 560, 160);
        g_window = [[NSWindow alloc] initWithContentRect:content
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        if (!g_window) return false;
        g_window.title = Text(title);
        g_window.releasedWhenClosed = NO;
        g_delegate = [[BuffyFirstStartDelegate alloc] init];
        g_window.delegate = g_delegate;
        NSView* view = g_window.contentView;
        const CGFloat margin = 20, width = content.size.width - 2 * margin;
        g_heading = Label(NSMakeRect(margin, 114, width, 22), NSTextAlignmentLeft, [NSColor labelColor]);
        g_stage = Label(NSMakeRect(margin, 90, width, 20), NSTextAlignmentLeft, [NSColor secondaryLabelColor]);
        g_bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(margin, 62, width, 16)];
        g_bar.style = NSProgressIndicatorStyleBar;
        g_bar.indeterminate = NO;
        g_bar.minValue = 0;
        g_bar.maxValue = 1;
        g_percent = Label(NSMakeRect(margin, 34, width / 2, 20), NSTextAlignmentLeft, [NSColor secondaryLabelColor]);
        g_left = Label(NSMakeRect(margin + width / 2, 34, width / 2, 20), NSTextAlignmentRight, [NSColor secondaryLabelColor]);
        for (NSView* v in @[ g_heading, g_stage, g_bar, g_percent, g_left ]) [view addSubview:v];
        [g_window center];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        g_activity = KEEP([[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityIdleSystemSleepDisabled
                                                                          reason:@"Getting the game ready"]);
        LOGR("first start: the progress window is open");  // the simulation (tools/mac/sim_first_start_mac.sh) checks that a windowless start never says it
        return true;
    }
}

void ProgressShow(const std::string& heading, const std::string& stage, double fraction, const std::string& time_left) {
    @autoreleasepool {
        if (!g_window) return;
        g_heading.stringValue = Text(heading);
        g_stage.stringValue = Text(stage);
        g_bar.doubleValue = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
        g_percent.stringValue = Text(std::to_string(int(fraction * 100.0)) + "%");
        g_left.stringValue = Text(time_left);
        [g_window displayIfNeeded];
        // the window's events (the close button, the menu bar) are SDL's to take: the caller's SDL_PollEvent does
    }
}

bool ProgressCloseRequested() { return g_close_requested.exchange(false); }

bool ProgressAskStop(const std::string& title, const std::string& question) {
    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = Text(title);
        alert.informativeText = Text(question);
        alert.alertStyle = NSAlertStyleInformational;
        [alert addButtonWithTitle:@"Keep going"];
        [alert addButtonWithTitle:@"Stop"];
        return [alert runModal] == NSAlertSecondButtonReturn;
    }
}

void ProgressClose() {
    @autoreleasepool {
        if (g_activity) {
            [[NSProcessInfo processInfo] endActivity:g_activity];
            g_activity = nil;
        }
        if (!g_window) return;
        g_window.delegate = nil;
        [g_window orderOut:nil];
        [g_window close];
        g_window = nil;
    }
}

}  // namespace host
