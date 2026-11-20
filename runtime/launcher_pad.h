/* The launcher's Controller tab: detected pads, rumble, dead zones, inversion, and a per-button remap with "press a button" capture. */
#import <Cocoa/Cocoa.h>
@interface StwPadTab : NSObject
@property (readonly) NSView *view;
- (instancetype)initWithSize:(NSSize)size;
- (void)stop;                                   /* call when the launcher closes: stops the timers */
@end
