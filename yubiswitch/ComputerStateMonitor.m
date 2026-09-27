//
//  ComputerStateMonitor.m
//  yubiswitch
//
//  Created by Angelo Failla on 8/29/15.
//  Copyright (c) 2015 Angelo Failla. All rights reserved.
//

#import "ComputerStateMonitor.h"
#import <Foundation/NSDistributedNotificationCenter.h>
#include <CoreGraphics/CGSession.h>

@interface ComputerStateMonitor ()
- (instancetype)initWithYubiKey:(YubiKey *)yubikey
              sessionDictionary:(NSDictionary *)sessionDictionary;
- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary
                                          atUptime:(NSTimeInterval)uptime;
- (void)retryAutomaticAction:(NSTimer *)timer;
- (void)retryAutomaticActionWithSessionDictionary:(NSDictionary *)sessionDictionary;
@end

@implementation ComputerStateMonitor

- (id)initWithYubiKey:(YubiKey *)yubikey {
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [self initWithYubiKey:yubikey sessionDictionary:session];
}

- (instancetype)initWithYubiKey:(YubiKey *)yubikey
              sessionDictionary:(NSDictionary *)sessionDictionary {
    self = [super init];
    if (self == nil || yubikey == nil) return nil;
    yk = yubikey;
    // The lock key is absent from some session dictionaries. Preserve the
    // key for login until an explicit unlock event resolves unknown state.
    id locked = sessionDictionary[@"CGSSessionScreenIsLocked"];
    screenLocked = ![locked isKindOfClass:[NSNumber class]] || [locked boolValue];

    NSDistributedNotificationCenter *center =
        [NSDistributedNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(receive:)
                  name:@"com.apple.screenIsLocked" object:nil];
    [center addObserver:self selector:@selector(receive:)
                  name:@"com.apple.screenIsUnlocked" object:nil];
    return self;
}

-(void) receive: (NSNotification*) notification {
    if ([[notification name] isEqualToString:@"com.apple.screenIsLocked"]) {
        screenLocked = YES;
    } else if ([[notification name] isEqualToString:@"com.apple.screenIsUnlocked"]) {
        screenLocked = NO;
    } else {
        return;
    }
    lockStateFromNotification = YES;
    hasUnlockedSessionObservation = NO;
    [self cancelAutomaticRetry];
    BOOL activated =
        [[NSUserDefaults standardUserDefaults] boolForKey:@"disableAtLockSleep"];

    if (!activated) {
        return;
    }

    NSLog(@"Received notification %s", [[notification name] UTF8String]);

    if ([[notification name]  isEqual:@"com.apple.screenIsLocked"]) {
        NSLog(@"Screen is locked, renabling yubikey");
        if (![yk enable]) [self scheduleAutomaticRetry];
    }

    if ([[notification name]  isEqual:@"com.apple.screenIsUnlocked"]) {
        NSLog(@"Screen is unlocked, disabling yubikey");
        if (![yk disable]) [self scheduleAutomaticRetry];
    }
}

- (void)scheduleAutomaticRetry {
    if (automaticRetryTimer != nil) return;
    automaticRetryTimer = [NSTimer timerWithTimeInterval:5
                                                 target:self
                                               selector:@selector(retryAutomaticAction:)
                                               userInfo:nil
                                                repeats:NO];
    [[NSRunLoop mainRunLoop] addTimer:automaticRetryTimer forMode:NSRunLoopCommonModes];
}

- (void)cancelAutomaticRetry {
    [automaticRetryTimer invalidate];
    automaticRetryTimer = nil;
}

- (void)retryAutomaticAction:(NSTimer *)timer {
    if (timer != automaticRetryTimer) return;
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    [self retryAutomaticActionWithSessionDictionary:session];
}

- (void)retryAutomaticActionWithSessionDictionary:(NSDictionary *)sessionDictionary {
    [self cancelAutomaticRetry];
    BOOL shouldDisable = [self allowsAutomaticDisableWithSessionDictionary:sessionDictionary];
    BOOL succeeded = shouldDisable ? [yk disable] : [yk enable];
    if (!succeeded || (!shouldDisable && screenLocked &&
                       hasUnlockedSessionObservation)) {
        [self scheduleAutomaticRetry];
    }
}

- (BOOL)isScreenLocked {
    return screenLocked;
}

- (BOOL)allowsAutomaticDisable {
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [self allowsAutomaticDisableWithSessionDictionary:session];
}

- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary {
    return [self allowsAutomaticDisableWithSessionDictionary:sessionDictionary
                                                   atUptime:[NSProcessInfo processInfo].systemUptime];
}

- (BOOL)allowsAutomaticDisableWithSessionDictionary:(NSDictionary *)sessionDictionary
                                          atUptime:(NSTimeInterval)uptime {
    if (![[NSUserDefaults standardUserDefaults] boolForKey:@"disableAtLockSleep"]) {
        hasUnlockedSessionObservation = NO;
        return YES;
    }
    id locked = sessionDictionary[@"CGSSessionScreenIsLocked"];
    if (![locked isKindOfClass:[NSNumber class]] || [locked boolValue]) {
        hasUnlockedSessionObservation = NO;
        return NO;
    }
    if (lockStateFromNotification && screenLocked) {
        // A lock event can precede the session snapshot. Require the current
        // session to remain unlocked across a timer retry before overriding it.
        if (!hasUnlockedSessionObservation) {
            unlockedSessionObservedAt = uptime;
            hasUnlockedSessionObservation = YES;
            return NO;
        }
        if (uptime - unlockedSessionObservedAt < 5) return NO;
        screenLocked = NO;
    }
    return YES;
}

- (void)dealloc {
    [automaticRetryTimer invalidate];
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
}

@end
