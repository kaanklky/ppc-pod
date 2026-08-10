#import <Cocoa/Cocoa.h>
#include <CoreServices/CoreServices.h>
#include <stdio.h>

#include "login_items.h"

/* NSURL fileURLWithPath: (not NSBundle's -bundleURL, a 10.6+ addition -
 * this project targets the real 10.5 SDK) toll-free bridges to CFURLRef
 * for LSSharedFileListInsertItemURL/CFEqual below - standard pre-ARC
 * technique, no __bridge needed since this era predates ARC entirely. */
static int already_registered(LSSharedFileListRef list, CFURLRef appURL)
{
    UInt32 seed = 0;
    CFArrayRef snapshot = LSSharedFileListCopySnapshot(list, &seed);
    CFIndex i, count;
    int found = 0;

    if (snapshot == NULL) return 0;

    count = CFArrayGetCount(snapshot);
    for (i = 0; i < count && !found; i++) {
        LSSharedFileListItemRef item = (LSSharedFileListItemRef)CFArrayGetValueAtIndex(snapshot, i);
        CFURLRef itemURL = NULL;
        if (LSSharedFileListItemResolve(item, 0, &itemURL, NULL) == noErr && itemURL != NULL) {
            if (CFEqual(itemURL, appURL)) found = 1;
            CFRelease(itemURL);
        }
    }

    CFRelease(snapshot);
    return found;
}

void register_self_as_login_item(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSString *bundlePath = [[NSBundle mainBundle] bundlePath];
    NSURL *appURLObj;
    CFURLRef appURL;
    LSSharedFileListRef loginItems;

    if (bundlePath == nil) {
        fprintf(stderr, "[login_items] no bundle path (not running from a real .app bundle?) - skipping\n");
        [pool release];
        return;
    }
    appURLObj = [NSURL fileURLWithPath:bundlePath];
    appURL = (CFURLRef)appURLObj;

    loginItems = LSSharedFileListCreate(NULL, kLSSharedFileListSessionLoginItems, NULL);
    if (loginItems == NULL) {
        fprintf(stderr, "[login_items] LSSharedFileListCreate failed\n");
        [pool release];
        return;
    }

    if (already_registered(loginItems, appURL)) {
        fprintf(stderr, "[login_items] already registered as a login item\n");
    } else {
        LSSharedFileListItemRef newItem = LSSharedFileListInsertItemURL(
            loginItems, kLSSharedFileListItemLast, NULL, NULL, appURL, NULL, NULL);
        if (newItem != NULL) {
            fprintf(stderr, "[login_items] registered as a login item\n");
        } else {
            fprintf(stderr, "[login_items] LSSharedFileListInsertItemURL failed\n");
        }
    }

    CFRelease(loginItems);
    [pool release];
}
