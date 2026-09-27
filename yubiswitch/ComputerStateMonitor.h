//
//  ComputerStateMonitor.h
//  yubiswitch
//
//  Created by Angelo Failla on 8/29/15.
//  Copyright (c) 2015 Angelo Failla. All rights reserved.
//

#import <Foundation/Foundation.h>
#import "YubiKey.h"

@interface ComputerStateMonitor : NSObject {
    YubiKey* yk;
    BOOL screenLocked;
    BOOL lockStateFromNotification;
    BOOL hasUnlockedSessionObservation;
    NSTimeInterval unlockedSessionObservedAt;
    NSTimer *automaticRetryTimer;
}

- (id)initWithYubiKey:(YubiKey *)yubikey;
- (BOOL)isScreenLocked;
- (BOOL)allowsAutomaticDisable;
- (void)scheduleAutomaticRetry;
- (void)cancelAutomaticRetry;

@end
