// Lumo's graphics backend for macOS: one window, immediate-mode drawing into a
// Core Graphics bitmap, shown once per frame. Uses only system frameworks.
// Built into lumo by `make` on macOS (see Makefile); lumo.c calls the gfx_* functions.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static NSWindow *win;
static NSView *view;
static CGContextRef ctx;
static double W, H;
static CGFloat scale = 1;
static bool closed;
static NSMutableSet *held, *pressed_now, *pressed_frame;
static bool clicked_now, clicked_frame;
static double last_time, delta;
static long frames;
static NSAutoreleasePool *pool;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static NSString *key_name(NSEvent *e) {
  switch (e.keyCode) {
    case 123: return @"left";
    case 124: return @"right";
    case 125: return @"down";
    case 126: return @"up";
    case 49: return @"space";
    case 36: return @"enter";
    case 53: return @"escape";
    case 51: return @"backspace";
    case 48: return @"tab";
  }
  NSString *c = e.charactersIgnoringModifiers.lowercaseString;
  return c.length ? c : nil;
}

@interface LumoView : NSView
@end
@implementation LumoView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (void)keyDown:(NSEvent *)e {
  NSString *k = key_name(e);
  if (!k) return;
  if (!e.isARepeat) [pressed_now addObject:k];
  [held addObject:k];
}
- (void)keyUp:(NSEvent *)e {
  NSString *k = key_name(e);
  if (k) [held removeObject:k];
}
- (void)mouseDown:(NSEvent *)e { clicked_now = true; }
@end

@interface LumoDelegate : NSObject <NSWindowDelegate>
@end
@implementation LumoDelegate
- (BOOL)windowShouldClose:(id)sender { closed = true; return YES; }
- (void)windowDidResignKey:(NSNotification *)n { [held removeAllObjects]; } // no stuck keys
@end

static void add_menu(void) { // so cmd-Q quits
  NSMenu *bar = [[NSMenu new] autorelease], *app = [[NSMenu new] autorelease];
  NSMenuItem *item = [[NSMenuItem new] autorelease];
  [bar addItem:item];
  [item setSubmenu:app];
  [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
  [NSApp setMainMenu:bar];
}

bool gfx_window(int w, int h, const char *title) {
  pool = [NSAutoreleasePool new];
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  add_menu();
  NSRect r = NSMakeRect(0, 0, w, h);
  win = [[NSWindow alloc] initWithContentRect:r
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
  if (!win) return false;
  win.title = [NSString stringWithUTF8String:title];
  win.delegate = [LumoDelegate new];
  win.releasedWhenClosed = NO;
  view = [[LumoView alloc] initWithFrame:r];
  view.wantsLayer = YES;
  win.contentView = view;
  [win center];
  [win makeKeyAndOrderFront:nil];
  [win makeFirstResponder:view];
  [NSApp finishLaunching];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop

  W = w;
  H = h;
  scale = win.backingScaleFactor;
  view.layer.contentsScale = scale;
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  ctx = CGBitmapContextCreate(NULL, (size_t)(w * scale), (size_t)(h * scale), 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(cs);
  if (!ctx) return false;
  CGContextTranslateCTM(ctx, 0, h * scale); // y grows downwards, in points
  CGContextScaleCTM(ctx, scale, -scale);
  CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
  CGContextFillRect(ctx, CGRectMake(0, 0, W, H));

  held = [NSMutableSet new];
  pressed_now = [NSMutableSet new];
  pressed_frame = [NSMutableSet new];
  last_time = now_s();
  return true;
}

bool gfx_screenshot(const char *path) {
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
  CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
  bool ok = false;
  if (dst) {
    CGImageDestinationAddImage(dst, img, NULL);
    ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
  }
  CFRelease(url);
  CGImageRelease(img);
  return ok;
}

// Shows what was drawn, waits for the next 1/60 s, then handles input.
// Returns false once the window is closed.
bool gfx_frame(void) {
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  view.layer.contents = (id)img;
  [CATransaction commit];
  CGImageRelease(img);

  // LUMO_SNAPSHOT=out.png LUMO_SNAPSHOT_FRAME=n saves frame n and quits (for tests and screenshots)
  // LUMO_RECORD=dir saves every second frame as dir/00001.png ... (for GIFs; time then runs at a steady 1/60 s)
  frames++;
  const char *rec = getenv("LUMO_RECORD");
  if (rec && frames % 2 == 0) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%05ld.png", rec, frames / 2);
    gfx_screenshot(path);
  }
  const char *snap = getenv("LUMO_SNAPSHOT");
  if (snap) {
    const char *at = getenv("LUMO_SNAPSHOT_FRAME");
    if (frames >= (at ? atol(at) : 2)) exit(gfx_screenshot(snap) ? 0 : 1);
  }

  double wait = last_time + 1.0 / 60 - now_s();
  if (wait > 0) usleep((useconds_t)(wait * 1e6));

  NSEvent *e;
  while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]))
    [NSApp sendEvent:e];
  [pressed_frame setSet:pressed_now];
  [pressed_now removeAllObjects];
  clicked_frame = clicked_now;
  clicked_now = false;

  double t = now_s();
  delta = rec ? 1.0 / 60 : t - last_time; /* recording is slower than real time; keep the game's clock steady */
  last_time = t;
  [pool drain];
  pool = [NSAutoreleasePool new];
  return !closed;
}

static void fill_color(const double *c) { CGContextSetRGBFillColor(ctx, c[0], c[1], c[2], c[3]); }

void gfx_clear(const double *c) {
  fill_color(c);
  CGContextFillRect(ctx, CGRectMake(0, 0, W, H));
}

void gfx_rect(double x, double y, double w, double h, double radius, const double *c) {
  fill_color(c);
  if (radius <= 0) { CGContextFillRect(ctx, CGRectMake(x, y, w, h)); return; }
  radius = fmin(radius, fmin(w, h) / 2);
  CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(x, y, w, h), radius, radius, NULL);
  CGContextAddPath(ctx, p);
  CGContextFillPath(ctx);
  CGPathRelease(p);
}

void gfx_circle(double x, double y, double r, const double *c) {
  fill_color(c);
  CGContextFillEllipseInRect(ctx, CGRectMake(x - r, y - r, 2 * r, 2 * r));
}

void gfx_line(double x1, double y1, double x2, double y2, double width, const double *c) {
  CGContextSetRGBStrokeColor(ctx, c[0], c[1], c[2], c[3]);
  CGContextSetLineWidth(ctx, width);
  CGContextSetLineCap(ctx, kCGLineCapRound);
  CGContextMoveToPoint(ctx, x1, y1);
  CGContextAddLineToPoint(ctx, x2, y2);
  CGContextStrokePath(ctx);
}

static NSDictionary *text_attrs(double size, const double *c) {
  NSFont *font = [NSFont systemFontOfSize:size weight:NSFontWeightBold];
  NSColor *color = [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:c[3]];
  return @{NSFontAttributeName : font, NSForegroundColorAttributeName : color};
}

void gfx_text(const char *s, double x, double y, double size, const double *c) {
  NSGraphicsContext *g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES];
  [NSGraphicsContext saveGraphicsState];
  [NSGraphicsContext setCurrentContext:g];
  [[NSString stringWithUTF8String:s] drawAtPoint:NSMakePoint(x, y) withAttributes:text_attrs(size, c)];
  [NSGraphicsContext restoreGraphicsState];
}

double gfx_text_width(const char *s, double size) {
  static const double white[4] = {1, 1, 1, 1};
  return [[NSString stringWithUTF8String:s] sizeWithAttributes:text_attrs(size, white)].width;
}

bool gfx_key(const char *name) { return [held containsObject:[NSString stringWithUTF8String:name]]; }
bool gfx_pressed(const char *name) { return [pressed_frame containsObject:[NSString stringWithUTF8String:name]]; }

void gfx_mouse(double *x, double *y, bool *down, bool *clicked) {
  NSPoint p = [win mouseLocationOutsideOfEventStream];
  *x = p.x;
  *y = H - p.y;
  *down = ([NSEvent pressedMouseButtons] & 1) != 0;
  *clicked = clicked_frame;
}

void gfx_pixels(const unsigned char *rgba, int w, int h) { // stretched over the window, crisp pixels
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, rgba, (size_t)w * h * 4, NULL);
  CGImageRef img = CGImageCreate(w, h, 8, 32, (size_t)w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast, dp, NULL, false,
                                 kCGRenderingIntentDefault);
  CGContextSaveGState(ctx);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
  CGContextTranslateCTM(ctx, 0, H); // images draw bottom-up; undo the y flip
  CGContextScaleCTM(ctx, 1, -1);
  CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), img);
  CGContextRestoreGState(ctx);
  CGImageRelease(img);
  CGDataProviderRelease(dp);
  CGColorSpaceRelease(cs);
}

double gfx_delta(void) { return delta; }

bool gfx_sound(const char *name) { // macOS system sounds: Pop, Tink, Glass, Basso, Funk, Hero, ...
  NSSound *s = [NSSound soundNamed:[NSString stringWithUTF8String:name]];
  if (!s) return false;
  [s stop];
  [s play];
  return true;
}
