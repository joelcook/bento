/* bento_mac.m — macOS-only Cocoa hook that sets the running process's
 * dock / Cmd-Tab icon. raylib's SetWindowIcon doesn't reach the dock on
 * macOS — that comes from [NSApp setApplicationIconImage:] when launching
 * unbundled, or from the .app bundle's Info.plist when launched via
 * Launch Services. This file gives us the unbundled case. */

#import <Cocoa/Cocoa.h>
#include <stddef.h>

/* Pixel data is RGBA8, top-to-bottom, no premultiplication, matching the
 * layout raylib's GenImageColor produces. */
void bentoMacSetDockIcon(const unsigned char *rgba, int width, int height) {
  if (!rgba || width <= 0 || height <= 0)
    return;

  @autoreleasepool {
    unsigned char *plane = (unsigned char *)rgba;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:&plane
                      pixelsWide:width
                      pixelsHigh:height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSCalibratedRGBColorSpace
                     bytesPerRow:width * 4
                    bitsPerPixel:32];
    if (!rep)
      return;

    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [img addRepresentation:rep];
    [NSApp setApplicationIconImage:img];
  }
}

static NSOpenPanel *bentoPathPanel = nil;
static NSString *bentoChosenPath = nil;
static int bentoPickerState = 0; /* 0 idle, 1 active, 2 chosen, 3 canceled */

int bentoMacPathPickerActive(void) {
  return bentoPickerState == 1;
}

int bentoMacBeginChoosePath(int choose_dirs, const char *start_dir) {
  if (bentoPickerState == 1)
    return 0;

  @autoreleasepool {
    NSOpenPanel *panel = [[NSOpenPanel openPanel] retain];
    if (!panel)
      return 0;

    if (bentoChosenPath) {
      [bentoChosenPath release];
      bentoChosenPath = nil;
    }

    bentoPathPanel = panel;
    bentoPickerState = 1;

    if (![NSApp isActive])
      [NSApp activateIgnoringOtherApps:YES];

    [panel setAllowsMultipleSelection:NO];
    [panel setCanCreateDirectories:NO];
    [panel setCanChooseFiles:choose_dirs ? NO : YES];
    [panel setCanChooseDirectories:choose_dirs ? YES : NO];
    [panel setResolvesAliases:YES];

    if (start_dir && start_dir[0]) {
      NSString *path = [NSString stringWithUTF8String:start_dir];
      BOOL is_dir = NO;
      if (path && [[NSFileManager defaultManager] fileExistsAtPath:path
                                                       isDirectory:&is_dir]) {
        NSString *dir = is_dir ? path : [path stringByDeletingLastPathComponent];
        NSURL *url = [NSURL fileURLWithPath:dir isDirectory:YES];
        if (url)
          [panel setDirectoryURL:url];
      }
    }

    [panel beginWithCompletionHandler:^(NSInteger result) {
      if (result == NSModalResponseOK) {
        NSURL *url = [[bentoPathPanel URLs] firstObject];
        if (url) {
          bentoChosenPath = [[url path] retain];
          bentoPickerState = bentoChosenPath ? 2 : 3;
        } else {
          bentoPickerState = 3;
        }
      } else {
        bentoPickerState = 3;
      }

      [bentoPathPanel release];
      bentoPathPanel = nil;
    }];
    [panel makeKeyAndOrderFront:nil];
    return 1;
  }
}

int bentoMacPollChosenPath(char *out, size_t out_len) {
  if (!out || out_len == 0)
    return 0;

  @autoreleasepool {
    if (bentoPickerState == 0 || bentoPickerState == 1)
      return 0;
    if (bentoPickerState == 3) {
      bentoPickerState = 0;
      return -1;
    }

    if (!bentoChosenPath ||
        ![bentoChosenPath getFileSystemRepresentation:out maxLength:out_len]) {
      if (bentoChosenPath) {
        [bentoChosenPath release];
        bentoChosenPath = nil;
      }
      bentoPickerState = 0;
      return -1;
    }
    out[out_len - 1] = '\0';
    [bentoChosenPath release];
    bentoChosenPath = nil;
    bentoPickerState = 0;
    return 1;
  }
}
