// The frame rate in the corner of the window, F7 while playing: a layer of macOS's own over the game's picture, so it needs
// nothing from the renderer and shows in full screen too. Apple's Metal overlay can't do this: it only follows
// MTL_HUD_ENABLED at launch, and CAMetalLayer's developerHUDProperties neither started nor hid it (macOS 27, MoltenVK).
// The number is the frames the game showed over the last second, as FPS_LOG logs. Window thread.
#import <QuartzCore/QuartzCore.h>

#include <cstdio>

#include "host.h"

namespace host {

namespace {

CATextLayer* g_text = nil;

// the top right corner, as the window may have changed size
void Place() {
    const CGRect outer = g_text.superlayer.bounds;
    const CGFloat width = 84, height = 20, margin = 12;
    const CGFloat y = g_text.superlayer.geometryFlipped ? margin : outer.size.height - height - margin;
    g_text.frame = CGRectMake(outer.size.width - width - margin, y, width, height);
}

}  // namespace

bool FpsOverlayShown() { return g_text != nil; }

void FpsOverlayShow(void* metal_layer, bool show) {
    CALayer* parent = (__bridge CALayer*)metal_layer;
    if (!parent || show == FpsOverlayShown()) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (show) {
        g_text = [[CATextLayer alloc] init];
        g_text.contentsScale = parent.contentsScale;
        g_text.font = (__bridge CFTypeRef) @"Menlo-Bold";
        g_text.fontSize = 15;
        g_text.alignmentMode = kCAAlignmentCenter;
        g_text.string = @"-- fps";
        CGColorRef white = CGColorCreateGenericRGB(1, 1, 1, 1), panel = CGColorCreateGenericRGB(0, 0, 0, 0.65);
        g_text.foregroundColor = white;
        g_text.backgroundColor = panel;
        CGColorRelease(white);
        CGColorRelease(panel);
        g_text.cornerRadius = 6;
        [parent addSublayer:g_text];
        Place();
    } else {
        [g_text removeFromSuperlayer];
        [g_text release];
        g_text = nil;
    }
    [CATransaction commit];
}

void FpsOverlayUpdate(double fps) {
    if (!g_text) return;
    char text[32];
    std::snprintf(text, sizeof text, "%.0f fps", fps);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    g_text.string = [NSString stringWithUTF8String:text];
    Place();
    [CATransaction commit];
}

}  // namespace host
