/* macOS side of the invite links (src/hub/invite.cpp): retrohub:// URLs arrive as an Apple Event (kInternetEventClass / kAEGetURL)
 * from LaunchServices, not on the command line. The handler forwards the URL into the same queue the Unix socket feeds
 * (invite_deliver_url). The Info.plist half (CFBundleURLTypes with the "retrohub" scheme) belongs to packaging (lane 9).
 * Also the clipboard (NSPasteboard) for invite_copy_text. */
#import <Cocoa/Cocoa.h>

#include "invite.h"

@interface RHURLEventHandler : NSObject
- (void)handleURLEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply;
@end

@implementation RHURLEventHandler
- (void)handleURLEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply {
    (void)reply;
    NSString* url = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
    if (!url) return;
    invite_deliver_url(std::string([url UTF8String]));
}
@end

static RHURLEventHandler* g_handler = nil;

void invite_install_url_handler() {
    if (g_handler) return;
    g_handler = [[RHURLEventHandler alloc] init];
    /* SDL2 registers its own kAEGetURL handler when the video subsystem starts (it turns URLs into drop events); setting ours
     * afterwards replaces it, so call this after SDL_Init(SDL_INIT_VIDEO). */
    [[NSAppleEventManager sharedAppleEventManager] setEventHandler:g_handler
                                                       andSelector:@selector(handleURLEvent:withReplyEvent:)
                                                     forEventClass:kInternetEventClass
                                                        andEventID:kAEGetURL];
}

bool invite_copy_text_utf8(const std::string& text_utf8) {
    @autoreleasepool {
        NSString* s = [NSString stringWithUTF8String:text_utf8.c_str()];
        if (!s) return false;
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        return [pb setString:s forType:NSPasteboardTypeString] == YES;
    }
}
