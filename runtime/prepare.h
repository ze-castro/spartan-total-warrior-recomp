/* First-run preparation of the app: turns the user's own disc into <data dir>/lifted/<game>-<build>/lifted.dylib
 * (analysis of default.xbe, x86 -> C translation, clang). Nothing derived from the game ever ships with the app. */
#ifndef STW_PREPARE_H
#define STW_PREPARE_H
#include <stddef.h>
int stw_lifted_ready(void);       /* 1 when the translated code for this game and runtime build exists (always 1 in a developer build that links it in) */
typedef void (*StwPrepLog)(const char *line, void *ctx);
typedef void (*StwPrepStep)(int index, int total, const char *title, void *ctx);
/* Runs every step on the calling thread. *cancel is polled; returns 0 on success, otherwise fills err. game_root holds default.xbe. */
int stw_prepare_game(const char *game_root, StwPrepLog log, StwPrepStep step, volatile int *cancel, void *ctx, char *err, size_t errlen);
#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
/* The launcher's "Preparing the game" panel: progress, current step and the log of the tools. */
@interface StwPrepareView : NSObject
@property (strong) NSView *box;
@property (strong) NSTextField *title, *stepLabel;
@property (strong) NSProgressIndicator *bar;
@property (strong) NSTextView *logView;
@property (strong) NSButton *cancelBtn, *retryBtn;
@property (assign) BOOL busy;
@property (assign) volatile int cancelFlag;
@property (copy) void (^completion)(BOOL ok);
@property (copy) NSString *root;
- (instancetype)initWithParent:(NSView *)parent frame:(NSRect)frame;
- (void)startWithGameRoot:(const char *)root completion:(void (^)(BOOL ok))done;
@end
#endif
#endif
