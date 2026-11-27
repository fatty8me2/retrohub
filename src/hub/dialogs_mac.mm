/* macOS file / folder panels and the Reduce Motion setting for the hub (see dialogs_mac.h). */
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "dialogs_mac.h"

#include <string>
#include <vector>

namespace {

/* "*.gba;*.zip" -> {"gba", "zip"}; "*.*", "*" or "" -> {} (any file). */
std::vector<std::string> spec_extensions(const std::string& spec) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= spec.size()) {
        size_t j = spec.find(';', i);
        if (j == std::string::npos) j = spec.size();
        std::string tok = spec.substr(i, j - i);
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        if (tok.size() > 2 && tok[0] == '*' && tok[1] == '.' && tok.find('*', 2) == std::string::npos) out.push_back(tok.substr(2));
        else if (tok == "*.*" || tok == "*") return {};
        i = j + 1;
    }
    return out;
}

NSArray<UTType*>* types_for(const std::vector<std::string>& exts) {
    NSMutableArray<UTType*>* a = [NSMutableArray array];
    for (const std::string& e : exts) {
        UTType* t = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:e.c_str()]];
        if (t) [a addObject:t];
    }
    return a;
}

template <class F>
auto on_main(F&& f) -> decltype(f()) {
    __block decltype(f()) r{};
    if ([NSThread isMainThread]) { r = f(); return r; }
    dispatch_sync(dispatch_get_main_queue(), ^{ r = f(); });
    return r;
}

}  // namespace

std::string mac_pick_path(bool folder, const std::string& filter_spec) {
    return on_main([&]() -> std::string {
        @autoreleasepool {
            NSOpenPanel* p = [NSOpenPanel openPanel];
            p.canChooseFiles = !folder;
            p.canChooseDirectories = folder;
            p.allowsMultipleSelection = NO;
            p.canCreateDirectories = folder;
            if (!folder) {
                NSArray<UTType*>* t = types_for(spec_extensions(filter_spec));
                if (t.count > 0) p.allowedContentTypes = t;
            }
            [NSApp activateIgnoringOtherApps:YES];
            if ([p runModal] != NSModalResponseOK || p.URL == nil) return std::string();
            const char* s = p.URL.path.UTF8String;
            return s ? std::string(s) : std::string();
        }
    });
}

std::string mac_pick_save_path(const std::string& default_name, const std::string& filter_spec, const std::string& default_ext) {
    return on_main([&]() -> std::string {
        @autoreleasepool {
            NSSavePanel* p = [NSSavePanel savePanel];
            p.canCreateDirectories = YES;
            std::vector<std::string> exts = spec_extensions(filter_spec);
            if (exts.empty() && !default_ext.empty()) exts.push_back(default_ext);
            NSArray<UTType*>* t = types_for(exts);
            if (t.count > 0) p.allowedContentTypes = t;
            if (!default_name.empty()) p.nameFieldStringValue = [NSString stringWithUTF8String:default_name.c_str()];
            [NSApp activateIgnoringOtherApps:YES];
            if ([p runModal] != NSModalResponseOK || p.URL == nil) return std::string();
            const char* s = p.URL.path.UTF8String;
            return s ? std::string(s) : std::string();
        }
    });
}

bool mac_reduce_motion() {
    @autoreleasepool {
        return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion] ? true : false;
    }
}

bool mac_copy_image_file_to_clipboard(const std::string& path) {
    @autoreleasepool {
        NSImage* img = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
        if (!img) return false;
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        return [pb writeObjects:@[ img ]] ? true : false;
    }
}

std::vector<MacWindow> mac_windows_of_pids(const std::vector<int>& pids, bool include_offscreen) {
    std::vector<MacWindow> out;
    if (pids.empty()) return out;
    @autoreleasepool {
        CGWindowListOption opt = include_offscreen ? kCGWindowListOptionAll : kCGWindowListOptionOnScreenOnly;
        CFArrayRef arr = CGWindowListCopyWindowInfo(opt | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (!arr) return out;
        for (NSDictionary* d in (__bridge NSArray*)arr) {
            const int pid = [d[(__bridge NSString*)kCGWindowOwnerPID] intValue];
            bool mine = false;
            for (int p : pids) if (p == pid) { mine = true; break; }
            if (!mine) continue;
            if ([d[(__bridge NSString*)kCGWindowLayer] intValue] != 0) continue;
            CGRect r = CGRectZero;
            NSDictionary* b = d[(__bridge NSString*)kCGWindowBounds];
            if (!b || !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)b, &r)) continue;
            MacWindow w;
            w.id = (unsigned)[d[(__bridge NSString*)kCGWindowNumber] unsignedIntValue];
            w.pid = pid;
            w.w = (int)r.size.width;
            w.h = (int)r.size.height;
            NSNumber* on = d[(__bridge NSString*)kCGWindowIsOnscreen];
            w.on_screen = on ? [on boolValue] : false;
            out.push_back(w);
        }
        CFRelease(arr);
    }
    return out;
}

int mac_frontmost_pid() {
    @autoreleasepool {
        NSRunningApplication* a = [[NSWorkspace sharedWorkspace] frontmostApplication];
        return a ? (int)a.processIdentifier : 0;
    }
}

bool mac_request_quit(int pid) {
    @autoreleasepool {
        NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:(pid_t)pid];
        if (!a || a.activationPolicy == NSApplicationActivationPolicyProhibited) return false;
        return [a terminate] ? true : false;
    }
}

void mac_bounce_dock() {
    @autoreleasepool {
        if (![NSThread isMainThread]) return;
        if ([NSApp isActive]) return;
        [NSApp requestUserAttention:NSInformationalRequest];
    }
}
