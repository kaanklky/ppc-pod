#import <Cocoa/Cocoa.h>

#include <string.h>

#include "cocoa_ui.h"
#include "app_state.h"
#include "app_settings.h"

#define PPC_POD_APP_TITLE @"PowerPC Pod"

/* With a plain NSView as the window's contentView, clicking the window
 * background never resigns the device-name NSTextField's first-responder
 * status once focused - AppKit only resigns first responder when
 * something else explicitly becomes first responder, and a plain NSView
 * doesn't accept that role by default. Making the background view itself
 * acceptFirstResponder and claim it on mouseDown: gives clicking empty
 * window space the expected "unfocus" behavior. */
@interface PPCBackgroundView : NSView
@end

@implementation PPCBackgroundView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)mouseDown:(NSEvent *)event
{
    [[self window] makeFirstResponder:self];
}
@end

/* Custom-drawn album art well: rounded corners and a recessed inner-shadow
 * look, neither of which NSImageView's built-in frame styles can give us
 * with a precisely-controlled size (NSImageFrameGrayBezel's own decorative
 * inset is a fixed AppKit constant, not something callers can adjust or
 * measure). Drawing it all ourselves means the image, the border, and the
 * shadow all agree on exactly the same rounded-rect geometry. */
@interface PPCArtView : NSView
{
    NSImage *image;
}
- (void)setImage:(NSImage *)img;
- (NSImage *)image;
@end

/* NSBezierPath's +bezierPathWithRoundedRect:xRadius:yRadius: convenience
 * constructor doesn't exist before 10.5 - this is the same four-arc
 * construction AppKit itself used internally pre-Leopard, built from
 * +appendBezierPathWithArcWithCenter:radius:startAngle:endAngle:, which has
 * been part of NSBezierPath since 10.0, so it draws identically on every
 * target OS version. */
static NSBezierPath *PPCRoundedRectPath(NSRect rect, CGFloat radius)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    NSRect innerRect = NSInsetRect(rect, radius, radius);
    [path appendBezierPathWithArcWithCenter:NSMakePoint(NSMinX(innerRect), NSMinY(innerRect)) radius:radius startAngle:180.0f endAngle:270.0f];
    [path appendBezierPathWithArcWithCenter:NSMakePoint(NSMaxX(innerRect), NSMinY(innerRect)) radius:radius startAngle:270.0f endAngle:360.0f];
    [path appendBezierPathWithArcWithCenter:NSMakePoint(NSMaxX(innerRect), NSMaxY(innerRect)) radius:radius startAngle:0.0f endAngle:90.0f];
    [path appendBezierPathWithArcWithCenter:NSMakePoint(NSMinX(innerRect), NSMaxY(innerRect)) radius:radius startAngle:90.0f endAngle:180.0f];
    [path closePath];
    return path;
}

/* NSGradient doesn't exist before 10.5 either - this hand-rolled banding
 * approximates the same linear alpha fade (dark at the outer edge, fading
 * to transparent over shadowDepth) by stacking thin translucent strips,
 * which is exactly what NSGradient itself does under the hood anyway. */
static void PPCDrawFadeBand(NSRect rect, BOOL vertical, BOOL fadeFromMaxEdge)
{
    const NSInteger steps = 8;
    const CGFloat startAlpha = 0.35f;
    NSInteger i;

    for (i = 0; i < steps; i++) {
        CGFloat t = (CGFloat)i / (CGFloat)steps;
        CGFloat alpha = startAlpha * (1.0f - t);
        NSRect strip;

        if (vertical) {
            CGFloat stripHeight = rect.size.height / steps;
            CGFloat y = fadeFromMaxEdge ? (rect.origin.y + rect.size.height - (i + 1) * stripHeight)
                                         : (rect.origin.y + i * stripHeight);
            strip = NSMakeRect(rect.origin.x, y, rect.size.width, stripHeight);
        } else {
            CGFloat stripWidth = rect.size.width / steps;
            CGFloat x = fadeFromMaxEdge ? (rect.origin.x + rect.size.width - (i + 1) * stripWidth)
                                         : (rect.origin.x + i * stripWidth);
            strip = NSMakeRect(x, rect.origin.y, stripWidth, rect.size.height);
        }

        [[NSColor colorWithCalibratedWhite:0.0 alpha:alpha] set];
        NSRectFillUsingOperation(strip, NSCompositeSourceOver);
    }
}

@implementation PPCArtView

- (void)setImage:(NSImage *)img
{
    [image autorelease];
    image = [img retain];
    [self setNeedsDisplay:YES];
}

- (NSImage *)image
{
    return image;
}

- (void)dealloc
{
    [image release];
    [super dealloc];
}

- (void)drawRect:(NSRect)dirtyRect
{
    NSRect bounds = [self bounds];
    CGFloat radius = 8.0f;
    NSBezierPath *path = PPCRoundedRectPath(bounds, radius);

    [[NSColor colorWithCalibratedWhite:0.82 alpha:1.0] set];
    [path fill];

    [NSGraphicsContext saveGraphicsState];
    [path addClip];

    if (image != nil) {
        NSSize imgSize = [image size];
        if (imgSize.width > 0 && imgSize.height > 0) {
            /* Aspect-fill: scale so the image covers the whole well (may
             * crop slightly on one axis) rather than aspect-fit, which
             * would show letterboxing inside the rounded rect - a filled
             * square well reads better for cover art than a windowboxed
             * one. */
            CGFloat scale = MAX(bounds.size.width / imgSize.width, bounds.size.height / imgSize.height);
            NSSize drawSize = NSMakeSize(imgSize.width * scale, imgSize.height * scale);
            NSRect drawRect = NSMakeRect(bounds.origin.x + (bounds.size.width - drawSize.width) / 2.0f,
                                          bounds.origin.y + (bounds.size.height - drawSize.height) / 2.0f,
                                          drawSize.width, drawSize.height);
            [image drawInRect:drawRect fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:1.0f];
        }
    }

    /* Inner shadow on all four edges, fading inward - the classic Aqua
     * "recessed well" look (search fields, scroll views, etc. of this
     * era), applied on top of the image so the well reads as a real
     * physical recess rather than a flat sticker. */
    {
        CGFloat shadowDepth = 8.0f;
        NSRect topRect = NSMakeRect(bounds.origin.x, bounds.origin.y + bounds.size.height - shadowDepth,
                                     bounds.size.width, shadowDepth);
        NSRect bottomRect = NSMakeRect(bounds.origin.x, bounds.origin.y,
                                        bounds.size.width, shadowDepth);
        NSRect leftRect = NSMakeRect(bounds.origin.x, bounds.origin.y,
                                      shadowDepth, bounds.size.height);
        NSRect rightRect = NSMakeRect(bounds.origin.x + bounds.size.width - shadowDepth, bounds.origin.y,
                                       shadowDepth, bounds.size.height);
        PPCDrawFadeBand(topRect, YES, YES);
        PPCDrawFadeBand(bottomRect, YES, NO);
        PPCDrawFadeBand(leftRect, NO, NO);
        PPCDrawFadeBand(rightRect, NO, YES);
    }

    [NSGraphicsContext restoreGraphicsState];

    [[NSColor colorWithCalibratedWhite:0.55 alpha:1.0] set];
    [path setLineWidth:1.0f];
    [path stroke];
}

@end

/* Manual alloc/init + [app run], no NSApplicationMain/.xib - no Interface
 * Builder in this cross-compile toolchain, and this era's AppKit has no
 * ARC, so every alloc here is matched with an explicit release. */
@interface PPCSpotiController : NSObject
{
    NSWindow *window;
    NSTextField *nameField;
    NSButton *saveButton;
    PPCArtView *artView;
    NSTextField *titleLabel;
    NSTextField *artistAlbumLabel;
    NSTextField *connectedInfoLabel;

    NSString *lastSavedName;
    NSString *lastArtPath;
}
- (void)buildUI;
- (void)pollBackend:(NSTimer *)timer;
- (void)saveClicked:(id)sender;
- (void)controlTextDidChange:(NSNotification *)note;
- (void)windowWillClose:(NSNotification *)note;
@end

@implementation PPCSpotiController

- (void)buildUI
{
    /* Pure now-playing display (art/title/artist/album/connection info) -
     * this app has no local media control at all, playback is driven
     * entirely by whichever AirPlay sender is connected. */
    NSRect frame = NSMakeRect(120, 120, 260, 368);
    char settings_path[1024];
    char device_name[APP_SETTINGS_DEVICE_NAME_MAX];

    app_settings_default_path(settings_path, sizeof(settings_path));
    if (app_settings_read(settings_path, device_name, sizeof(device_name)) != 0) {
        /* First launch, no settings.txt yet - default the device name to
         * this Mac's hostname. The user can edit and Save at any time;
         * app_settings_read above will then find the saved name on every
         * later launch instead of this fallback. */
        app_settings_hostname_default(device_name, sizeof(device_name));
    }
    lastSavedName = [[NSString stringWithUTF8String:device_name] retain];
    lastArtPath = [[NSString alloc] init];
    app_state_set_device_name(app_state_shared(), device_name);

    window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask)
        backing:NSBackingStoreBuffered
        defer:NO];
    [window setTitle:PPC_POD_APP_TITLE];
    [window setDelegate:self];

    {
        PPCBackgroundView *bg = [[[PPCBackgroundView alloc]
            initWithFrame:[[window contentView] frame]] autorelease];
        [window setContentView:bg];
    }

    /* Device name field + Save button, top row. */
    nameField = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 334, 172, 22)];
    [nameField setStringValue:lastSavedName];
    [nameField setDelegate:self];
    [[window contentView] addSubview:nameField];

    saveButton = [[NSButton alloc] initWithFrame:NSMakeRect(189, 334, 64, 22)];
    [saveButton setBezelStyle:NSRoundedBezelStyle];
    [saveButton setTitle:@"Save"];
    [saveButton setTarget:self];
    [saveButton setAction:@selector(saveClicked:)];
    [saveButton setEnabled:NO]; /* name is already what's on disk (or the hostname default) at launch */
    /* A 22pt-tall frame is shorter than NSRoundedBezelStyle's minimum
     * content height, clipping the bezel's own top edge - sizeToFit
     * computes the correct height, which then needs recentering against
     * nameField's vertical center since it's taller than nameField (22).
     * Width is reset back afterward since sizeToFit would also shrink it
     * to fit "Save" tightly. The -2pt below true center is a deliberate
     * visual adjustment against nameField, per direct feedback. */
    [saveButton sizeToFit];
    {
        NSRect sf = [saveButton frame];
        CGFloat nameCenterY = 334.0f + 22.0f / 2.0f;
        sf.origin.x = 189;
        sf.size.width = 64;
        sf.origin.y = nameCenterY - sf.size.height / 2.0f - 2.0f;
        [saveButton setFrame:sf];
    }
    [[window contentView] addSubview:saveButton];

    /* Album art - same left/right margins (12pt) as the name/Save row above
     * and the title/artist labels below, so its edges actually line up
     * with the rest of the column instead of overhanging it. A previous
     * pass grew this box past that column (to 8..252) trying to compensate
     * for NSImageFrameGrayBezel's fixed decorative inset, which is what
     * actually made the Save button look off-center relative to the art
     * beneath it - the row itself was already correctly margined. */
    artView = [[PPCArtView alloc] initWithFrame:NSMakeRect(12, 86, 236, 236)];
    [[window contentView] addSubview:artView];

    titleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 62, 236, 16)];
    [titleLabel setEditable:NO];
    [titleLabel setBordered:NO];
    [titleLabel setDrawsBackground:NO];
    [titleLabel setAlignment:NSCenterTextAlignment];
    [titleLabel setStringValue:@""];
    [[window contentView] addSubview:titleLabel];

    artistAlbumLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 46, 236, 14)];
    [artistAlbumLabel setEditable:NO];
    [artistAlbumLabel setBordered:NO];
    [artistAlbumLabel setDrawsBackground:NO];
    [artistAlbumLabel setAlignment:NSCenterTextAlignment];
    [artistAlbumLabel setTextColor:[NSColor darkGrayColor]];
    [artistAlbumLabel setFont:[NSFont systemFontOfSize:11]];
    [artistAlbumLabel setStringValue:@""];
    [[window contentView] addSubview:artistAlbumLabel];

    /* Bottom row: connected-info label, full width and right-aligned. */
    connectedInfoLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 8, 236, 18)];
    [connectedInfoLabel setEditable:NO];
    [connectedInfoLabel setBordered:NO];
    [connectedInfoLabel setDrawsBackground:NO];
    [connectedInfoLabel setAlignment:NSRightTextAlignment];
    [connectedInfoLabel setFont:[NSFont systemFontOfSize:10]];
    [connectedInfoLabel setTextColor:[NSColor darkGrayColor]];
    [connectedInfoLabel setStringValue:@""];
    [[window contentView] addSubview:connectedInfoLabel];

    [window makeKeyAndOrderFront:nil];

    [NSTimer scheduledTimerWithTimeInterval:0.4
                                      target:self
                                    selector:@selector(pollBackend:)
                                    userInfo:nil
                                     repeats:YES];
}

- (void)saveClicked:(id)sender
{
    char settings_path[1024];
    NSString *text = [nameField stringValue];

    app_settings_default_path(settings_path, sizeof(settings_path));
    /* Only treat the name as saved if the write genuinely succeeded; leave
     * Save enabled (and the on-screen device name unchanged) on failure so
     * a real problem stays visible instead of silently lying about it. */
    if (app_settings_write(settings_path, [text UTF8String]) != 0) {
        fprintf(stderr, "[cocoa_ui] failed to write settings to %s\n", settings_path);
        return;
    }
    app_state_set_device_name(app_state_shared(), [text UTF8String]);

    [lastSavedName release];
    lastSavedName = [text retain];
    [saveButton setEnabled:NO];
}

- (void)controlTextDidChange:(NSNotification *)note
{
    BOOL dirty = ![[nameField stringValue] isEqualToString:lastSavedName];
    [saveButton setEnabled:dirty];
}

- (void)pollBackend:(NSTimer *)timer
{
    app_state_snapshot snap;
    app_state_get_snapshot(app_state_shared(), &snap);

    if (snap.have_track) {
        [titleLabel setStringValue:[NSString stringWithUTF8String:snap.track.title]];
        /* NSString's "%s" format specifier does not reliably decode UTF-8
         * multi-byte sequences - convert each C string to an NSString via
         * stringWithUTF8String: first, then combine with "%@". */
        if (snap.track.artist[0] != '\0' && snap.track.album[0] != '\0') {
            NSString *artistStr = [NSString stringWithUTF8String:snap.track.artist];
            NSString *albumStr = [NSString stringWithUTF8String:snap.track.album];
            [artistAlbumLabel setStringValue:
                [NSString stringWithFormat:@"%@ - %@", artistStr, albumStr]];
        } else {
            [artistAlbumLabel setStringValue:[NSString stringWithUTF8String:
                snap.track.artist[0] != '\0' ? snap.track.artist : snap.track.album]];
        }

        {
            NSString *artPath = [NSString stringWithUTF8String:snap.track.album_art_path];
            if (![artPath isEqualToString:lastArtPath]) {
                [lastArtPath release];
                lastArtPath = [artPath retain];
                if ([artPath length] > 0) {
                    NSImage *img = [[NSImage alloc] initWithContentsOfFile:artPath];
                    /* NSImage's "size" property is derived from whatever
                     * DPI/resolution tag happens to be embedded in the
                     * file, which cover art sources don't serve
                     * consistently - an image whose logical size comes out
                     * smaller than the art box would render shrunk and
                     * off-center. Overriding the size to the image's real
                     * pixel dimensions makes every image's logical size
                     * match its actual pixel content. */
                    if (img != nil) {
                        NSArray *reps = [img representations];
                        if ([reps count] > 0) {
                            NSImageRep *rep = [reps objectAtIndex:0];
                            NSSize pixelSize = NSMakeSize([rep pixelsWide], [rep pixelsHigh]);
                            if (pixelSize.width > 0 && pixelSize.height > 0) {
                                [img setSize:pixelSize];
                            }
                        }
                    }
                    [artView setImage:img];
                    [img release];
                } else {
                    [artView setImage:nil];
                }
            }
        }

    } else {
        [titleLabel setStringValue:@""];
        [artistAlbumLabel setStringValue:@""];
        if ([lastArtPath length] > 0) {
            [lastArtPath release];
            lastArtPath = [[NSString alloc] init];
            [artView setImage:nil];
        }
    }

    /* AirPlay 1's handshake has no guaranteed friendly-name field for the
     * sender - a name is only shown when one is actually available. */
    if (snap.active_backend == APP_BACKEND_NONE) {
        [connectedInfoLabel setStringValue:@""];
    } else if (snap.peer_name[0] != '\0') {
        NSString *nameStr = [NSString stringWithUTF8String:snap.peer_name];
        [connectedInfoLabel setStringValue:
            [NSString stringWithFormat:@"%@ %s", nameStr, snap.peer_ip]];
    } else if (snap.peer_ip[0] != '\0') {
        [connectedInfoLabel setStringValue:[NSString stringWithUTF8String:snap.peer_ip]];
    } else {
        [connectedInfoLabel setStringValue:@"connected"];
    }
}

- (void)windowWillClose:(NSNotification *)note
{
    /* This app has no invisible background-service mode - closing the one
     * visible window means the app itself quits, matching the explicit
     * "no background service the user doesn't see" requirement. */
    [NSApp terminate:nil];
}

@end

void cocoa_ui_run(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *app = [NSApplication sharedApplication];
    PPCSpotiController *controller = [[PPCSpotiController alloc] init];

    [controller buildUI];
    [app activateIgnoringOtherApps:YES];
    [app run];

    [controller release];
    [pool release];
}
