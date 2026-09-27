#import <AppKit/AppKit.h>
#import "../yubiswitch/AppDelegate.h"
#include <assert.h>
#include <stdio.h>

@interface AppDelegate (TestAccess)
- (BOOL)applyInitialKeyPolicy;
@end

@interface FakeKey : NSObject
@property(nonatomic) NSUInteger enables;
@property(nonatomic) NSUInteger disables;
@property(nonatomic) BOOL disabled;
@property(nonatomic) BOOL stateKnown;
@property(nonatomic) BOOL failNextDisable;
@property(nonatomic) BOOL failNextEnable;
- (BOOL)enable;
- (BOOL)disable;
- (BOOL)isStateKnown;
- (BOOL)state;
@end

@implementation FakeKey
- (BOOL)enable {
    self.enables++;
    if (self.failNextEnable) { self.failNextEnable = NO; self.stateKnown = NO; return NO; }
    self.disabled = NO;
    self.stateKnown = YES;
    return YES;
}
- (BOOL)disable {
    self.disables++;
    if (self.failNextDisable) {
        self.failNextDisable = NO;
        self.stateKnown = NO;
        return NO;
    }
    self.disabled = YES;
    self.stateKnown = YES;
    return YES;
}
- (BOOL)isStateKnown { return self.stateKnown; }
- (BOOL)state { return self.disabled; }
@end

@interface FakeMonitor : NSObject
@property(nonatomic) BOOL allowsDisable;
@property(nonatomic) NSUInteger scheduledRetries;
@property(nonatomic) NSUInteger cancelledRetries;
- (BOOL)allowsAutomaticDisable;
- (void)scheduleAutomaticRetry;
- (void)cancelAutomaticRetry;
@end

@implementation FakeMonitor
- (BOOL)allowsAutomaticDisable { return self.allowsDisable; }
- (void)scheduleAutomaticRetry { self.scheduledRetries++; }
- (void)cancelAutomaticRetry { self.cancelledRetries++; }
@end

@interface FakeDefaultsController : NSObject
@property(nonatomic, strong) NSMutableDictionary *values;
- (void)save:(id)sender;
@end

@implementation FakeDefaultsController
- (void)save:(id)sender {
    [[NSUserDefaults standardUserDefaults]
        setVolatileDomain:@{@"switchOffDelay": self.values[@"switchOffDelay"]}
               forName:NSArgumentDomain];
}
@end

@interface TestDelegate : AppDelegate
@property(nonatomic) NSTimeInterval lastScheduledInterval;
@property(nonatomic) NSUInteger invalidDelayAlerts;
- (void)useKey:(FakeKey *)key monitor:(FakeMonitor *)monitor;
- (NSTimer *)pendingDisableTimer;
@end

@implementation TestDelegate
- (void)useKey:(FakeKey *)key monitor:(FakeMonitor *)monitor {
    yk = (YubiKey *)key;
    state_monitor = (ComputerStateMonitor *)monitor;
}
- (NSTimer *)pendingDisableTimer { return reDisableTimer; }
- (NSTimer *)createTimer:(NSTimeInterval)interval {
    self.lastScheduledInterval = interval;
    return [super createTimer:interval];
}
- (void)notify:(NSString *)message { (void)message; }
- (void)showInvalidDelayAlert { self.invalidDelayAlerts++; }
@end

int main(void) {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"switchOffDelay": @{@"enabled": @YES,
                                                       @"interval": @"10.0"}}
                   forName:NSArgumentDomain];
        TestDelegate *delegate = [TestDelegate new];
        FakeKey *key = [FakeKey new];
        FakeMonitor *monitor = [FakeMonitor new];
        [delegate useKey:key monitor:monitor];

        // Locked or unknown at launch: preserve the key for login.
        monitor.allowsDisable = NO;
        assert([delegate applyInitialKeyPolicy]);
        assert(key.enables == 1 && key.disables == 0);
        key.failNextEnable = YES;
        assert(![delegate applyInitialKeyPolicy]);
        assert(monitor.scheduledRetries == 1);

        // An unlocked launch follows the normal disabled policy.
        monitor.allowsDisable = YES;
        assert([delegate applyInitialKeyPolicy]);
        assert(key.enables == 2 && key.disables == 1);

        // A timed shutoff must remain pending while lock state is uncertain.
        key.disabled = NO;
        monitor.allowsDisable = NO;
        [delegate reDisableYK];
        assert(key.disables == 1);
        NSTimer *retry = [delegate pendingDisableTimer];
        assert(retry != nil && [retry isValid]);

        // Once the lock state allows it, the pending timer shuts the key off.
        monitor.allowsDisable = YES;
        [retry fire];
        assert(key.disables == 2 && key.disabled);
        assert([delegate pendingDisableTimer] == nil);
        monitor.allowsDisable = NO;
        [delegate reDisableYK];
        assert(key.disables == 2);
        assert([delegate pendingDisableTimer] == nil);

        // A helper error leaves the key on and must retain the shutoff intent.
        key.disabled = NO;
        key.failNextDisable = YES;
        monitor.allowsDisable = YES;
        [delegate reDisableYK];
        assert(key.disables == 3 && !key.disabled);
        retry = [delegate pendingDisableTimer];
        assert(retry != nil && [retry isValid]);
        [retry fire];
        assert(key.disables == 4 && key.disabled && key.stateKnown);
        assert([delegate pendingDisableTimer] == nil);
        assert([PreferencesController validatedSwitchOffInterval:@"10.0"] == 10);
        assert([PreferencesController validatedSwitchOffInterval:@"1.5"] == 1.5);
        assert([PreferencesController validatedSwitchOffInterval:@"0"] == 0);
        assert([PreferencesController validatedSwitchOffInterval:@"-2"] == 0);
        assert([PreferencesController validatedSwitchOffInterval:@"abc"] == 0);

        [delegate enableYubiKey:YES];
        assert(monitor.cancelledRetries > 0);
        assert([delegate pendingDisableTimer] != nil);
        // A direct action must use the current key state, even before the
        // status-bar refresh has caught up with an automatic change.
        key.disabled = YES;
        NSUInteger enablesBefore = key.enables;
        [delegate toggle:nil];
        assert(key.enables == enablesBefore + 1);
        assert(!key.disabled && [delegate status]);
        key.disabled = YES;
        assert(![delegate status]);
        [delegate enableYubiKey:NO];
        key.disabled = NO;
        NSUInteger disablesBeforeToggle = key.disables;
        [delegate toggle:nil];
        assert(key.disables == disablesBeforeToggle + 1);
        assert(key.disabled && ![delegate status]);
        NSUInteger cancelsBefore = monitor.cancelledRetries;
        key.failNextDisable = YES;
        [delegate enableYubiKey:NO];
        assert(monitor.cancelledRetries == cancelsBefore);
        [delegate enableYubiKey:YES];
        assert([delegate pendingDisableTimer] != nil);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"switchOffDelay": @{@"enabled": @NO,
                                                       @"interval": @"10.0"}}
                   forName:NSArgumentDomain];
        [delegate toggleSwitchOffDelay:nil];
        assert([delegate pendingDisableTimer] == nil);
        NSUInteger disablesBefore = key.disables;
        [delegate reDisableYK];
        assert(key.disables == disablesBefore);

        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"switchOffDelay": @{@"enabled": @YES,
                                                       @"interval": @"1.5"}}
                   forName:NSArgumentDomain];
        [delegate toggleSwitchOffDelay:nil];
        assert([delegate pendingDisableTimer] != nil);
        assert(delegate.lastScheduledInterval == 1.5);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"switchOffDelay": @{@"enabled": @YES,
                                                       @"interval": @"abc"}}
                   forName:NSArgumentDomain];
        [delegate toggleSwitchOffDelay:nil];
        assert([delegate pendingDisableTimer] == nil);
        FakeDefaultsController *fakeController = [FakeDefaultsController new];
        fakeController.values = [@{@"switchOffDelay": [@{@"enabled": @YES,
                                                         @"interval": @"abc"} mutableCopy]} mutableCopy];
        delegate.controller = (NSUserDefaultsController *)fakeController;
        [delegate toggleSwitchOffDelay:nil];
        assert(![fakeController.values[@"switchOffDelay"][@"enabled"] boolValue]);
        assert(delegate.invalidDelayAlerts == 1);
        assert([delegate pendingDisableTimer] == nil);
        puts("app_lock_policy_test: startup and timer recovery passed");
    }
    return 0;
}
