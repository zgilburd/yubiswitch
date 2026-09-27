#import <Foundation/Foundation.h>
#import "../yubiswitch/ComputerStateMonitor.h"
#include <assert.h>
#include <stdio.h>

@interface ComputerStateMonitor (TestAccess)
- (void)receive:(NSNotification *)notification;
- (BOOL)isScreenLocked;
- (BOOL)allowsAutomaticDisable;
- (instancetype)initWithYubiKey:(YubiKey *)key
              sessionDictionary:(NSDictionary *)sessionDictionary;
- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary;
- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary
                                          atUptime:(NSTimeInterval)uptime;
- (void)retryAutomaticActionWithSessionDictionary:(NSDictionary *)sessionDictionary;
@end

@interface TestMonitor : ComputerStateMonitor
- (NSTimer *)pendingAutomaticRetry;
@end

@implementation TestMonitor
- (NSTimer *)pendingAutomaticRetry { return automaticRetryTimer; }
@end

@interface FakeKey : NSObject
@property(nonatomic) NSUInteger enables;
@property(nonatomic) NSUInteger disables;
@property(nonatomic) BOOL failNextEnable;
@property(nonatomic) BOOL failNextDisable;
- (BOOL)enable;
- (BOOL)disable;
@end

@implementation FakeKey
- (BOOL)enable {
    self.enables++;
    if (self.failNextEnable) { self.failNextEnable = NO; return NO; }
    return YES;
}
- (BOOL)disable {
    self.disables++;
    if (self.failNextDisable) { self.failNextDisable = NO; return NO; }
    return YES;
}
@end

int main(void) {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @YES}
                   forName:NSArgumentDomain];
        FakeKey *key = [FakeKey new];
        ComputerStateMonitor *initiallyLocked = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @YES}];
        assert(![initiallyLocked allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}]);
        ComputerStateMonitor *initiallyUnlocked = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @NO}];
        assert([initiallyUnlocked allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        ComputerStateMonitor *unknown = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key sessionDictionary:nil];
        assert(![unknown allowsAutomaticDisableWithSessionDictionary:nil]);
        assert([unknown allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        ComputerStateMonitor *monitor = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @NO}];
        assert(![monitor isScreenLocked]);
        assert([monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        // A lock missed during startup must still block the timer callback.
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:nil]);
        [monitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsLocked" object:nil]];
        assert([monitor isScreenLocked]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}]);
        // A missed unlock notification must not keep a confirmed unlocked
        // session blocked forever. A conflicting locked read resets confidence.
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:100]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES} atUptime:101]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:102]);
        assert(![monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:106]);
        assert([monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:107]);
        assert(![monitor isScreenLocked]);
        assert(key.enables == 1);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @NO}
                   forName:NSArgumentDomain];
        assert([monitor allowsAutomaticDisableWithSessionDictionary:nil]);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @YES}
                   forName:NSArgumentDomain];
        [monitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsUnlocked" object:nil]];
        assert(![monitor isScreenLocked]);
        assert([monitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}]);
        assert(key.disables == 1);

        ComputerStateMonitor *preferenceChange = [[ComputerStateMonitor alloc]
            initWithYubiKey:(YubiKey *)key
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @NO}];
        [preferenceChange receive:[NSNotification notificationWithName:
            @"com.apple.screenIsLocked" object:nil]];
        assert(![preferenceChange allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:200]);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @NO}
                   forName:NSArgumentDomain];
        assert([preferenceChange allowsAutomaticDisableWithSessionDictionary:nil
                                                                    atUptime:201]);
        [[NSUserDefaults standardUserDefaults]
            setVolatileDomain:@{@"disableAtLockSleep": @YES}
                   forName:NSArgumentDomain];
        assert(![preferenceChange allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:207]);
        assert([preferenceChange allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO} atUptime:212]);

        FakeKey *retryKey = [FakeKey new];
        TestMonitor *retryMonitor = [[TestMonitor alloc]
            initWithYubiKey:(YubiKey *)retryKey
          sessionDictionary:@{@"CGSSessionScreenIsLocked": @NO}];
        retryKey.failNextEnable = YES;
        [retryMonitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsLocked" object:nil]];
        assert(retryKey.enables == 1);
        assert([retryMonitor pendingAutomaticRetry] != nil);
        [retryMonitor retryAutomaticActionWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @YES}];
        assert(retryKey.enables == 2);
        assert([retryMonitor pendingAutomaticRetry] == nil);

        retryKey.failNextEnable = YES;
        [retryMonitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsLocked" object:nil]];
        assert([retryMonitor pendingAutomaticRetry] != nil);
        [retryMonitor retryAutomaticActionWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}];
        assert([retryMonitor pendingAutomaticRetry] != nil);
        assert(![retryMonitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}
                                                    atUptime:[NSProcessInfo processInfo].systemUptime + 4]);
        assert([retryMonitor allowsAutomaticDisableWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}
                                                    atUptime:[NSProcessInfo processInfo].systemUptime + 6]);
        [retryMonitor retryAutomaticActionWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}];
        assert([retryMonitor pendingAutomaticRetry] == nil);

        retryKey.failNextDisable = YES;
        [retryMonitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsUnlocked" object:nil]];
        assert(retryKey.disables == 2);
        assert([retryMonitor pendingAutomaticRetry] != nil);
        [retryMonitor retryAutomaticActionWithSessionDictionary:
            @{@"CGSSessionScreenIsLocked": @NO}];
        assert(retryKey.disables == 3);
        assert([retryMonitor pendingAutomaticRetry] == nil);
        retryKey.failNextDisable = YES;
        [retryMonitor receive:[NSNotification notificationWithName:
            @"com.apple.screenIsUnlocked" object:nil]];
        NSTimer *cancelled = [retryMonitor pendingAutomaticRetry];
        assert(cancelled != nil);
        [retryMonitor cancelAutomaticRetry];
        assert([retryMonitor pendingAutomaticRetry] == nil);
        NSUInteger disablesBeforeCancel = retryKey.disables;
        [cancelled fire];
        assert(retryKey.disables == disablesBeforeCancel);
        puts("screen_lock_timer_test: lock state tracked");
    }
    return 0;
}
