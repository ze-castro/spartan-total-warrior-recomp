#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "prepare.h"
#include "paths.h"

int stw_lifted_ready(void) {
#ifdef STW_STATIC
    return 1;
#else
    char p[1300];
    stw_lifted_path(p, sizeof p);
    return access(p, R_OK) == 0;
#endif
}

static void mkdirs(const char *path) {
    char p[1300];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}

/* Runs one program, forwarding its output line by line. Returns its exit status, -1 if cancelled, -2 if it could not start. */
static int run_cmd(NSString *cwd, NSString *exe, NSArray<NSString *> *args, StwPrepLog log, volatile int *cancel, void *ctx) {
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:exe];
    t.arguments = args;
    if (cwd) t.currentDirectoryURL = [NSURL fileURLWithPath:cwd];
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    env[@"PYTHONDONTWRITEBYTECODE"] = @"1";
    env[@"PIP_DISABLE_PIP_VERSION_CHECK"] = @"1";
    t.environment = env;
    NSPipe *pipe = [NSPipe pipe];
    t.standardOutput = pipe; t.standardError = pipe;
    __block NSMutableString *pending = [NSMutableString new];
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData;
        if (!d.length) return;
        [pending appendString:[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @""];
        NSRange r;
        while ((r = [pending rangeOfString:@"\n"]).location != NSNotFound) {
            NSString *line = [pending substringToIndex:r.location];
            [pending deleteCharactersInRange:NSMakeRange(0, r.location + 1)];
            if (log) log(line.UTF8String, ctx);
        }
    };
    NSError *err = nil;
    if (![t launchAndReturnError:&err]) { if (log) log(err.localizedDescription.UTF8String, ctx); return -2; }
    while (t.isRunning) {
        if (cancel && *cancel) { [t terminate]; [t waitUntilExit]; pipe.fileHandleForReading.readabilityHandler = nil; return -1; }
        usleep(100000);
    }
    usleep(150000);                                       /* let the handler drain the last output */
    pipe.fileHandleForReading.readabilityHandler = nil;
    if (pending.length && log) log(pending.UTF8String, ctx);
    return t.terminationStatus;
}

static int have_cmdline_tools(void) {
    NSString *out = nil;
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcode-select"];
    t.arguments = @[@"-p"];
    NSPipe *p = [NSPipe pipe];
    t.standardOutput = p; t.standardError = [NSPipe pipe];
    if (![t launchAndReturnError:nil]) return 0;
    [t waitUntilExit];
    out = [[NSString alloc] initWithData:p.fileHandleForReading.readDataToEndOfFile encoding:NSUTF8StringEncoding];
    return t.terminationStatus == 0 && out.length && access("/usr/bin/clang", X_OK) == 0 && access("/usr/bin/python3", X_OK) == 0;
}

#define NSTEPS 8
int stw_prepare_game(const char *game_root, StwPrepLog log, StwPrepStep step, volatile int *cancel, void *ctx, char *err, size_t errlen) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *res = @(stw_resources_dir());
    if (![res hasPrefix:@"/"]) res = [fm.currentDirectoryPath stringByAppendingPathComponent:res];
    NSString *xbe = [@(game_root) stringByAppendingString:@"/default.xbe"];
    char key[200];
    snprintf(key, sizeof key, "%.8s-%.12s", stw_supported_xbe_hash(), stw_build_id());
    char wsp[1300], lib[1300], datadir[1300];
    stw_path(datadir, sizeof datadir, "");
    snprintf(wsp, sizeof wsp, "%s/work/%s", stw_data_dir(), key);
    stw_lifted_path(lib, sizeof lib);
    NSString *ws = @(wsp);
    NSString *libdir = [@(lib) stringByDeletingLastPathComponent];
#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)
#define STEP(i, title) do { if (step) step(i, NSTEPS, title, ctx); if (cancel && *cancel) FAIL("cancelled"); } while (0)

    STEP(0, "Checking the tools");
    if (!have_cmdline_tools()) FAIL("The Xcode command line tools are needed to prepare the game. Open Terminal, run:  xcode-select --install  then start the game again.");
    if (![fm fileExistsAtPath:[res stringByAppendingString:@"/tools/lift.py"]]) FAIL("The app's tools are missing (%s/tools). Rebuild the app or run from a checkout.", res.UTF8String);
    mkdirs(wsp); mkdirs(libdir.UTF8String);
    [fm removeItemAtPath:[ws stringByAppendingString:@"/tools"] error:nil];
    [fm removeItemAtPath:[ws stringByAppendingString:@"/runtime"] error:nil];
    [fm createSymbolicLinkAtPath:[ws stringByAppendingString:@"/tools"] withDestinationPath:[res stringByAppendingString:@"/tools"] error:nil];
    [fm createSymbolicLinkAtPath:[ws stringByAppendingString:@"/runtime"] withDestinationPath:[res stringByAppendingString:@"/runtime"] error:nil];
    mkdirs([ws stringByAppendingString:@"/analysis"].UTF8String);
    mkdirs([ws stringByAppendingString:@"/build"].UTF8String);

    STEP(1, "Setting up Python");
    NSString *venv = [NSString stringWithFormat:@"%s/venv", stw_data_dir()];
    NSString *py = [venv stringByAppendingString:@"/bin/python"];
    NSString *vendor = [res stringByAppendingString:@"/vendor"];
    int rc = 0;
    if (![fm isExecutableFileAtPath:py] || run_cmd(ws, py, @[@"-c", @"import capstone"], NULL, cancel, ctx) != 0) {
        [fm removeItemAtPath:venv error:nil];
        if ((rc = run_cmd(ws, @"/usr/bin/python3", @[@"-m", @"venv", venv], log, cancel, ctx))) FAIL(rc == -1 ? "cancelled" : "could not create the Python environment (python3 -m venv failed)");
        if ((rc = run_cmd(ws, py, @[@"-m", @"pip", @"install", @"--no-index", @"--find-links", vendor, @"capstone"], log, cancel, ctx)))
            FAIL(rc == -1 ? "cancelled" : "could not install capstone from %s (is the app complete?)", vendor.UTF8String);
    }

    NSArray *pipeline = @[
        @[@"Finding the functions in the game's executable", @"analysis/functions.json", @[@"-I", @"tools/discover.py", xbe, @"analysis"]],
        @[@"Naming the library functions", @"analysis/libsyms.json", @[@"tools/oovpa_match.py", xbe, @"analysis/libsyms.json"]],
        @[@"Translating the game's code to C", @"build/lifted.c", @[@"-I", @"tools/lift.py", xbe, @"analysis/functions.json", @"build/lifted.c"]],
        @[@"Generating the library tables", @"build/libstubs.c", @[@"tools/gen_libstubs.py", xbe, @"build/lifted.c.json", @"build/libstubs.c"]],
        @[@"Generating the kernel table", @"build/ktable.c", @[@"tools/gen_kernel.py", xbe, @"build/ktable.c"]],
    ];
    int idx = 2;
    for (NSArray *s in pipeline) {
        STEP(idx, [s[0] UTF8String]);
        NSString *out = [ws stringByAppendingPathComponent:s[1]];
        NSString *marker = [out stringByAppendingString:@".done"];
        if (![fm fileExistsAtPath:marker]) {                       /* a cancelled run resumes after the last finished step */
            if ((rc = run_cmd(ws, py, s[2], log, cancel, ctx))) FAIL(rc == -1 ? "cancelled" : "step failed: %s", [s[0] UTF8String]);
            if (![fm fileExistsAtPath:out]) FAIL("step produced no output: %s", [s[0] UTF8String]);
            [@"" writeToFile:marker atomically:NO encoding:NSUTF8StringEncoding error:nil];
        } else if (log) log([NSString stringWithFormat:@"(already done: %@)", s[0]].UTF8String, ctx);
        idx++;
    }
    STEP(7, "Compiling the translated code (the longest step)");
    NSString *tmp = [@(lib) stringByAppendingString:@".tmp"];
    NSArray *cc = @[@"-w", @"-ffp-contract=off", @"-O1", @"-shared", @"-undefined", @"dynamic_lookup", @"-I", [res stringByAppendingString:@"/runtime"], @"-I",
                    [ws stringByAppendingString:@"/build"], @"-o", tmp, [ws stringByAppendingString:@"/build/lifted.c"], [ws stringByAppendingString:@"/build/ktable.c"],
                    [ws stringByAppendingString:@"/build/libstubs.c"], [res stringByAppendingString:@"/runtime/gametables.c"]];
    if ((rc = run_cmd(ws, @"/usr/bin/clang", cc, log, cancel, ctx))) { [fm removeItemAtPath:tmp error:nil]; FAIL(rc == -1 ? "cancelled" : "the C compiler failed (see the log)"); }
    if (rename(tmp.UTF8String, lib)) FAIL("could not move the library into place");
    [fm removeItemAtPath:ws error:nil];                           /* the translated source is derived from the game: do not leave it lying around */
    if (step) step(NSTEPS, NSTEPS, "Done", ctx);
    return 0;
}


/* ---- the launcher's "Preparing the game" panel ---- */


static void prep_log(const char *line, void *ctx) {
    StwPrepareView *v = (__bridge StwPrepareView *)ctx;
    NSString *l = [NSString stringWithFormat:@"%s\n", line];
    dispatch_async(dispatch_get_main_queue(), ^{
        NSTextStorage *ts = v.logView.textStorage;
        [ts appendAttributedString:[[NSAttributedString alloc] initWithString:l attributes:@{ NSFontAttributeName: [NSFont userFixedPitchFontOfSize:10], NSForegroundColorAttributeName: NSColor.textColor }]];
        if (ts.length > 60000) [ts deleteCharactersInRange:NSMakeRange(0, ts.length - 40000)];
        [v.logView scrollRangeToVisible:NSMakeRange(ts.length, 0)];
    });
    if (getenv("STW_PREPLOG")) fprintf(stderr, "[prepare] %s\n", line);
}
static void prep_step(int i, int n, const char *title, void *ctx) {
    StwPrepareView *v = (__bridge StwPrepareView *)ctx;
    NSString *t = [NSString stringWithFormat:@"Step %d of %d: %s", i < n ? i + 1 : n, n, title];
    dispatch_async(dispatch_get_main_queue(), ^{ v.stepLabel.stringValue = t; v.bar.doubleValue = (double)i / n * 100.0; });
    if (getenv("STW_PREPLOG")) fprintf(stderr, "[prepare] === %s\n", t.UTF8String);
}
@implementation StwPrepareView
- (instancetype)initWithParent:(NSView *)parent frame:(NSRect)f {
    self = [super init];
    self.box = [[NSView alloc] initWithFrame:f];
    self.box.hidden = YES;
    CGFloat w = f.size.width, h = f.size.height;
    self.title = [NSTextField labelWithString:@"Preparing the game for this Mac (about 5 minutes, only once)"];
    self.title.font = [NSFont boldSystemFontOfSize:13];
    self.title.frame = NSMakeRect(0, h - 20, w, 18);
    self.stepLabel = [NSTextField labelWithString:@""];
    self.stepLabel.frame = NSMakeRect(0, h - 40, w - 160, 16);
    self.bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, h - 58, w - 90, 12)];
    self.bar.style = NSProgressIndicatorStyleBar; self.bar.indeterminate = NO; self.bar.minValue = 0; self.bar.maxValue = 100;
    self.cancelBtn = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    self.cancelBtn.frame = NSMakeRect(w - 80, h - 66, 80, 26);
    self.retryBtn = [NSButton buttonWithTitle:@"Retry" target:self action:@selector(retry:)];
    self.retryBtn.frame = NSMakeRect(w - 80, h - 66, 80, 26); self.retryBtn.hidden = YES;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, w, h - 72)];
    sv.hasVerticalScroller = YES; sv.borderType = NSBezelBorder;
    self.logView = [[NSTextView alloc] initWithFrame:sv.bounds];
    self.logView.editable = NO; self.logView.minSize = NSMakeSize(0, 0);
    self.logView.maxSize = NSMakeSize(1e7, 1e7); self.logView.verticallyResizable = YES; self.logView.autoresizingMask = NSViewWidthSizable;
    self.logView.textContainer.widthTracksTextView = YES;
    sv.documentView = self.logView;
    for (NSView *v in @[self.title, self.stepLabel, self.bar, self.cancelBtn, self.retryBtn, sv]) [self.box addSubview:v];
    [parent addSubview:self.box];
    return self;
}
- (void)cancel:(id)sender { (void)sender; self.cancelFlag = 1; }
- (void)retry:(id)sender { (void)sender; [self startWithGameRoot:self.root.UTF8String completion:self.completion]; }
- (void)startWithGameRoot:(const char *)root completion:(void (^)(BOOL ok))done {
    if (self.busy) return;
    self.root = @(root);
    self.completion = done;
    self.busy = YES; self.cancelFlag = 0;
    self.box.hidden = NO; self.retryBtn.hidden = YES; self.cancelBtn.hidden = NO;
    self.title.stringValue = @"Preparing the game for this Mac (about 5 minutes, only once)";
    self.title.textColor = NSColor.labelColor;
    self.bar.doubleValue = 0;
    [self.logView setString:@""];
    NSString *r = @(root);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char err[500] = "";
        int rc = stw_prepare_game(r.UTF8String, prep_log, prep_step, &self->_cancelFlag, (__bridge void *)self, err, sizeof err);
        NSString *e = @(err);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.busy = NO;
            if (rc == 0) { self.box.hidden = YES; }
            else {
                self.title.stringValue = [NSString stringWithFormat:@"Could not prepare the game: %@", e];
                self.title.textColor = [NSColor colorWithCalibratedRed:0.75 green:0.2 blue:0.1 alpha:1];
                self.cancelBtn.hidden = YES; self.retryBtn.hidden = NO;
            }
            if (self.completion) self.completion(rc == 0);
        });
    });
}
@end
