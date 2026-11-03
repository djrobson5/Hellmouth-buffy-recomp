// What a level restart needs of the window and of AppKit that SDL doesn't offer (main.cpp's handover). Window thread.
#import <AppKit/AppKit.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "host.h"

namespace host {

namespace {

NSWindow* Of(SDL_Window* window) {
    return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                                      nullptr);
}

}  // namespace

// A window that's there but can't be seen draws as any other (the renderer's presents go on), which hidden or behind
// another it might not. Without the animation it opens in one frame, over the old window in the same place.
void WindowSeen(SDL_Window* window, bool seen) {
    NSWindow* w = Of(window);
    if (!w) return;
    w.animationBehavior = NSWindowAnimationBehaviorNone;
    w.alphaValue = seen ? 1.0 : 0.0;
}

}  // namespace host

// what the green button and Window > Enter Full Screen (Control-Command-F) do instead of macOS's full screen
@interface BuffyFullScreenTarget : NSObject
@property(nonatomic) void (*toggle)();
@end

@implementation BuffyFullScreenTarget
- (void)toggle:(id)sender {
    (void)sender;
    if (self.toggle) self.toggle();
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    (void)item;
    return YES;
}
@end

namespace host {

// Full screen without a Space of its own (SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES off) leaves the green button zooming the
// window and the menu's item doing nothing. Both are given the game's own full screen.
void TakeFullScreenControls(SDL_Window* window, void (*toggle)()) {
    static BuffyFullScreenTarget* target = [[BuffyFullScreenTarget alloc] init];
    target.toggle = toggle;
    NSWindow* w = Of(window);
    if (NSButton* zoom = [w standardWindowButton:NSWindowZoomButton]) {
        zoom.target = target;
        zoom.action = @selector(toggle:);
    }
    NSMenuItem* full = nil;
    for (NSMenuItem* top in NSApp.mainMenu.itemArray)
        for (NSMenuItem* item in top.submenu.itemArray)
            if (item.action == @selector(toggleFullScreen:)) full = item;
    if (!full && NSApp.windowsMenu) full = [NSApp.windowsMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    if (!full) return;
    full.target = target;
    full.action = @selector(toggle:);
    full.title = @"Toggle Full Screen";
    full.keyEquivalent = @"f";
    full.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
}

// SDL hides the pointer only once it has seen the mouse come over its window, which a window opening under a pointer that
// stays still never shows it (each level's new window). So it's hidden here, while it's over the window's picture and the
// app is in front, and shown again as soon as it isn't.
void HidePointer(SDL_Window* window, bool want) {
    static bool hidden = false;
    static int seen = -1;
    NSWindow* w = Of(window);
    if (want && w) {
        const NSRect picture = [w convertRectToScreen:[w contentRectForFrameRect:w.frame]];
        const bool active = NSApp.active, key = w.isKeyWindow, over = NSPointInRect(NSEvent.mouseLocation, picture);
        if (const int now = active | key << 1 | over << 2; now != seen) {
            seen = now;
            LOGI("pointer: app %s, window %s, pointer %s the picture", active ? "active" : "not active", key ? "key" : "not key",
                 over ? "over" : "off");
        }
        want = active && key && over;
    }
    if (want == hidden) return;
    hidden = want;
    if (want) [NSCursor hide];
    else [NSCursor unhide];
}

// The next level's instance, through Launch Services as an app of its own. Started straight from this process, macOS would
// count it as part of this app, which then stays in the Dock and in a launcher's list of running apps after it has gone. Any thread.
int OpenInstance(const std::string& exe, const std::vector<std::string>& args) {
    NSURL* app = [[[[NSURL fileURLWithPath:@(exe.c_str())] URLByDeletingLastPathComponent] URLByDeletingLastPathComponent]
        URLByDeletingLastPathComponent];
    if (![app.pathExtension isEqualToString:@"app"]) return -1;
    NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
    config.createsNewApplicationInstance = YES;
    config.activates = NO;  // it comes forward itself, once its window shows
    config.addsToRecentItems = NO;
    config.promptsUserIfNeeded = NO;
    NSMutableArray<NSString*>* list = [NSMutableArray array];
    for (const std::string& a : args) [list addObject:@(a.c_str())];
    config.arguments = list;
    config.environment = NSProcessInfo.processInfo.environment;  // with what the restart has added
    __block int pid = -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [NSWorkspace.sharedWorkspace openApplicationAtURL:app
                                        configuration:config
                                    completionHandler:^(NSRunningApplication* started, NSError* error) {
                                        if (started) pid = started.processIdentifier;
                                        else NSLog(@"relaunch: %@", error);
                                        dispatch_semaphore_signal(done);
                                    }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
    return pid;
}

// macOS 14 and later only let an app come forward when the active one gives way to it
void YieldActivation(int pid) {
    NSRunningApplication* next = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!next) return;
    if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:next];
}

// the old instance, still in front, brings the next one forward as it goes: asked for by the next one alone, macOS 14 and
// later didn't give it the front (after yieldActivation too)
void ActivateOther(int pid) {
    NSRunningApplication* next = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (!next) return;
    if (@available(macOS 14.0, *)) [next activateFromApplication:NSRunningApplication.currentApplication options:0];
    else [next activateWithOptions:0];
}

// once the app is in front, its window takes the keys, as it was shown while another app was
void TakeKeys(SDL_Window* window) {
    NSWindow* w = Of(window);
    if (w && NSApp.active && !w.isKeyWindow && w.isVisible) [w makeKeyAndOrderFront:nil];
}

void Activate() {
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
}

}  // namespace host
