#import "launcher_pad.h"
#include "input.h"

@interface StwPadTab ()
@property (strong) NSView *view;
@property (strong) NSTextField *status, *hint;
@property (strong) NSButton *rumble, *invRy, *invLy, *swap;
@property (strong) NSSlider *rumbleStrength, *dzLeft, *dzRight, *sensRight, *trig;
@property (strong) NSMutableArray<NSTextField *> *valueLabels;
@property (strong) NSMutableArray<NSPopUpButton *> *rows;
@property (strong) NSTimer *poll;
@property (assign) int recording;
@property (assign) NSTimeInterval recordStart;
@end

@implementation StwPadTab
@synthesize view = _view;

static NSString *pct(double v) { return [NSString stringWithFormat:@"%.0f%%", v * 100]; }

- (NSTextField *)label:(NSString *)t at:(NSRect)r in:(NSView *)v bold:(BOOL)b {
    NSTextField *l = [NSTextField labelWithString:t];
    l.frame = r;
    if (b) l.font = [NSFont boldSystemFontOfSize:13];
    [v addSubview:l];
    return l;
}
- (NSSlider *)slider:(NSString *)name min:(double)lo max:(double)hi value:(double)v y:(CGFloat)y in:(NSView *)doc {
    [self label:name at:NSMakeRect(10, y + 3, 200, 18) in:doc bold:NO];
    NSSlider *s = [NSSlider sliderWithValue:v minValue:lo maxValue:hi target:self action:@selector(optionChanged:)];
    s.frame = NSMakeRect(215, y, 190, 24);
    [doc addSubview:s];
    NSTextField *vl = [self label:pct(v) at:NSMakeRect(412, y + 3, 56, 18) in:doc bold:NO];
    [self.valueLabels addObject:vl];
    return s;
}
- (NSButton *)check:(NSString *)t on:(BOOL)on y:(CGFloat)y in:(NSView *)doc {
    NSButton *b = [NSButton checkboxWithTitle:t target:self action:@selector(optionChanged:)];
    b.frame = NSMakeRect(10, y, 450, 22);
    b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    [doc addSubview:b];
    return b;
}

- (NSString *)titleForOut:(int)out {
    StwPadCfg c = stw_settings_get().pad;
    int s = c.map[out];
    return s < 0 ? @"(none)" : @(stw_pad_src_name(s));
}
- (void)refreshRow:(int)out {
    [self.rows[out].itemArray[0] setTitle:[self titleForOut:out]];
    [self.rows[out] selectItemAtIndex:0];
}
- (void)buildRows:(NSView *)doc topY:(CGFloat)top {
    self.rows = [NSMutableArray new];
    for (int o = 0; o < PO_COUNT; o++) {
        CGFloat y = top - (o + 1) * 30;
        [self label:@(stw_pad_out_name(o)) at:NSMakeRect(10, y + 6, 200, 18) in:doc bold:NO];
        NSPopUpButton *pb = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(215, y + 2, 250, 26) pullsDown:YES];
        [pb addItemWithTitle:@""];
        NSMenuItem *press = [pb.menu addItemWithTitle:@"Press a button..." action:@selector(rowMenu:) keyEquivalent:@""];
        press.target = self; press.tag = o * 1000 + 1;
        NSMenuItem *none = [pb.menu addItemWithTitle:@"None" action:@selector(rowMenu:) keyEquivalent:@""];
        none.target = self; none.tag = o * 1000 + 2;
        [pb.menu addItem:[NSMenuItem separatorItem]];
        for (int s = 0; s < PS_COUNT; s++) {
            NSMenuItem *mi = [pb.menu addItemWithTitle:@(stw_pad_src_name(s)) action:@selector(rowMenu:) keyEquivalent:@""];
            mi.target = self; mi.tag = o * 1000 + 10 + s;
        }
        [doc addSubview:pb];
        [self.rows addObject:pb];
        [self refreshRow:o];
    }
}

- (instancetype)initWithSize:(NSSize)size {
    if (!(self = [super init])) return nil;
    stw_pad_start();
    self.recording = -1;
    self.valueLabels = [NSMutableArray new];
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    self.view = v;
    StwPadCfg c = stw_settings_get().pad;
    self.status = [self label:@"" at:NSMakeRect(16, size.height - 36, size.width - 32, 34) in:v bold:NO];
    self.status.lineBreakMode = NSLineBreakByWordWrapping;
    self.status.maximumNumberOfLines = 2;
    self.status.preferredMaxLayoutWidth = size.width - 32;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(8, 40, size.width - 16, size.height - 84)];
    sv.hasVerticalScroller = YES; sv.borderType = NSBezelBorder;
    CGFloat docH = 14 * 30 + 16 * 30 + 80;
    NSView *doc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, docH)];
    CGFloat y = docH - 34;
    self.rumble = [self check:@"Rumble" on:c.rumble y:y in:doc]; y -= 30;
    self.rumbleStrength = [self slider:@"Rumble strength" min:0 max:1 value:c.rumble_strength y:y in:doc]; y -= 30;
    self.dzLeft = [self slider:@"Left stick dead zone" min:0 max:0.3 value:c.dz_left y:y in:doc]; y -= 30;
    self.dzRight = [self slider:@"Right stick (camera) dead zone" min:0 max:0.3 value:c.dz_right y:y in:doc]; y -= 30;
    self.sensRight = [self slider:@"Camera stick sensitivity" min:0.5 max:2.0 value:c.sens_right y:y in:doc]; y -= 30;
    self.trig = [self slider:@"Trigger counts as a press at" min:0.05 max:0.95 value:c.trig_thresh y:y in:doc]; y -= 30;
    self.invRy = [self check:@"Invert camera stick vertically" on:c.invert_ry y:y in:doc]; y -= 26;
    self.invLy = [self check:@"Invert left stick vertically" on:c.invert_ly y:y in:doc]; y -= 26;
    self.swap = [self check:@"Swap A/B and X/Y (Nintendo-style layout)" on:c.swap_face y:y in:doc]; y -= 40;
    [self label:@"Buttons: the game's button, then the controller input that triggers it" at:NSMakeRect(10, y + 8, 470, 16) in:doc bold:YES].font = [NSFont boldSystemFontOfSize:11];
    [self buildRows:doc topY:y + 4];
    sv.documentView = doc;
    [doc scrollPoint:NSMakePoint(0, docH)];
    [v addSubview:sv];
    NSButton *reset = [NSButton buttonWithTitle:@"Reset controller settings" target:self action:@selector(resetAll:)];
    reset.frame = NSMakeRect(8, 6, 190, 28);
    [v addSubview:reset];
    self.hint = [self label:@"Names use Xbox positions: A = bottom button (Cross), B = right (Circle), X = left (Square), Y = top (Triangle)." at:NSMakeRect(204, 4, size.width - 212, 32) in:v bold:NO];
    self.hint.font = [NSFont systemFontOfSize:10];
    self.hint.lineBreakMode = NSLineBreakByWordWrapping;
    self.hint.maximumNumberOfLines = 2;
    self.hint.preferredMaxLayoutWidth = size.width - 212;
    [self updateValueLabels];
    [self refreshStatus];
    self.poll = [NSTimer scheduledTimerWithTimeInterval:0.05 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.currentRunLoop addTimer:self.poll forMode:NSModalPanelRunLoopMode];       /* the launcher runs as a modal panel */
    return self;
}

- (void)updateValueLabels {
    NSArray<NSSlider *> *sl = @[self.rumbleStrength, self.dzLeft, self.dzRight, self.sensRight, self.trig];
    for (NSUInteger i = 0; i < sl.count; i++) self.valueLabels[i].stringValue = pct(sl[i].doubleValue);
}
- (void)refreshStatus {
    int n = stw_pad_count();
    NSMutableString *s = [NSMutableString string];
    if (!n) [s appendString:@"No controller detected: connect one (USB or Bluetooth) and it appears here. The keyboard and mouse always work."];
    else {
        [s appendString:@"Detected: "];
        for (int i = 0; i < n; i++) [s appendFormat:@"%@%s (port %d)", i ? @", " : @"", stw_pad_name(i), i + 1];
        [s appendString:@". The game uses controller 1."];
    }
    if (![self.status.stringValue isEqualToString:s]) self.status.stringValue = s;
}
- (void)tick:(NSTimer *)t {
    (void)t;
    static int n;
    if ((++n % 10) == 0) [self refreshStatus];
    if (self.recording < 0) return;
    int s = stw_pad_capture();
    if (s >= 0) {
        StwSettings c = stw_settings_get();
        c.pad.map[self.recording] = s;
        stw_settings_put(&c);
        int o = self.recording; self.recording = -1;
        [self refreshRow:o];
    } else if ([NSDate timeIntervalSinceReferenceDate] - self.recordStart > 8) {
        int o = self.recording; self.recording = -1;
        [self refreshRow:o];
    }
}
- (void)optionChanged:(id)sender {
    (void)sender;
    StwSettings c = stw_settings_get();
    c.pad.rumble = self.rumble.state == NSControlStateValueOn;
    c.pad.rumble_strength = (float)self.rumbleStrength.doubleValue;
    c.pad.dz_left = (float)self.dzLeft.doubleValue;
    c.pad.dz_right = (float)self.dzRight.doubleValue;
    c.pad.sens_right = (float)self.sensRight.doubleValue;
    c.pad.trig_thresh = (float)self.trig.doubleValue;
    c.pad.invert_ry = self.invRy.state == NSControlStateValueOn;
    c.pad.invert_ly = self.invLy.state == NSControlStateValueOn;
    c.pad.swap_face = self.swap.state == NSControlStateValueOn;
    stw_settings_put(&c);
    [self updateValueLabels];
}
- (void)rowMenu:(NSMenuItem *)mi {
    int out = (int)mi.tag / 1000, op = (int)mi.tag % 1000;
    int prev = self.recording;
    self.recording = -1;
    if (prev >= 0) [self refreshRow:prev];
    if (op == 1) {
        if (!stw_pad_count()) { [self.rows[out].itemArray[0] setTitle:@"(no controller connected)"]; return; }
        self.recording = out;
        self.recordStart = [NSDate timeIntervalSinceReferenceDate];
        [self.rows[out].itemArray[0] setTitle:@"press a button on the controller..."];
        return;
    }
    StwSettings c = stw_settings_get();
    c.pad.map[out] = op == 2 ? -1 : op - 10;
    stw_settings_put(&c);
    [self refreshRow:out];
}
- (void)resetAll:(id)sender {
    (void)sender;
    StwSettings c = stw_settings_get();
    stw_pad_defaults(&c.pad);
    stw_settings_put(&c);
    self.rumble.state = c.pad.rumble ? NSControlStateValueOn : NSControlStateValueOff;
    self.rumbleStrength.doubleValue = c.pad.rumble_strength;
    self.dzLeft.doubleValue = c.pad.dz_left; self.dzRight.doubleValue = c.pad.dz_right;
    self.sensRight.doubleValue = c.pad.sens_right; self.trig.doubleValue = c.pad.trig_thresh;
    self.invRy.state = self.invLy.state = self.swap.state = NSControlStateValueOff;
    for (int o = 0; o < PO_COUNT; o++) [self refreshRow:o];
    [self updateValueLabels];
}
- (void)stop { [self.poll invalidate]; self.poll = nil; }
@end
