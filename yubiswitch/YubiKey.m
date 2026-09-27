//  YubiKey.m
//  yubiswitch

/*
 yubiswitch - enable/disable yubikey
 Copyright (C) 2013-2015  Angelo "pallotron" Failla <pallotron@freaknet.org>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#import "YubiKey.h"
// This class is responsible for communicating with the helper process, which
// itself controls the USB device

#include <IOKit/hid/IOHIDManager.h>
#include <stdint.h>

@interface YubiKey ()
- (BOOL)action:(NSString *)action vendorID:(NSString *)vendor
       productID:(NSString *)product;
- (BOOL)registerKeyRemovalWithVendorID:(NSString *)vendor
                               productID:(NSString *)product;
@end

@implementation YubiKey

- (id)init {
    if (self = [super init]) {
        // Listen to notifications with name "changeDefaultsPrefs" and associate
        // notificationReloadHandler to it, this is the mechanism used to
        // communicate to this that UserDefaults preferences have changed,
        // typically when user hits the OK button in the Preference window.
        [[NSNotificationCenter defaultCenter]
         addObserver:self
         selector:@selector(notificationReloadHandler:)
         name:@"changeDefaultsPrefs"
         object:nil];
        if (!AXIsProcessTrusted()) {
            [self raiseAlertWindow:@"yubiswitch requires accessibility access to function. Please enable it in the Security and Privacy prefpane. Taking you there now."];
            NSString* prefPage = @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility";
            [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:prefPage]];
        }

        if ([self needToInstallHelper:@"com.zgilburd.yubiswitch.helper"]) {
            NSError *error = nil;
            if (![self blessHelperWithLabel:@"com.zgilburd.yubiswitch.helper"
                                      error:&error]) {
                [self raiseAlertWindow:
                 [NSString stringWithFormat:@"Failed to bless helper. Error: %@",
                  error]];
                exit(EXIT_FAILURE);
            }
        }
        lockOnRemoval = YES;
        if (![self registerKeyRemoval]) {
            [self raiseAlertWindow:@"Could not monitor YubiKey removal. The unplug lock is unavailable until device monitoring succeeds."];
        }
    }
    return self;

}

- (BOOL)needToInstallHelper:(NSString*) label {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSDictionary* installedHelperJobData =
    (NSDictionary*)CFBridgingRelease(SMJobCopyDictionary(kSMDomainSystemLaunchd, (__bridge CFStringRef)label));
#pragma clang diagnostic pop
    NSLog(@"Helper information: %@", installedHelperJobData);

    NSString* installedPath = nil;
    NSArray* programArguments = [installedHelperJobData objectForKey:@"ProgramArguments"];
    if ([programArguments isKindOfClass:[NSArray class]] && [programArguments count] > 0) {
        installedPath = [programArguments objectAtIndex:0];
    }

    // On newer macOS versions SMJobCopyDictionary may return nil for this helper.
    // In that case, check the canonical blessed helper path directly.
    if (![installedPath isKindOfClass:[NSString class]] || [installedPath length] == 0) {
        installedPath = [@"/Library/PrivilegedHelperTools" stringByAppendingPathComponent:label];
        NSLog(@"Falling back to helper path check: %@", installedPath);
    }

    NSURL* installedPathURL = [NSURL fileURLWithPath:installedPath];
    NSDictionary* installedInfoPlist =
        (NSDictionary*)CFBridgingRelease(CFBundleCopyInfoDictionaryForURL((CFURLRef)installedPathURL));
    NSString* installedBundleVersion = [installedInfoPlist objectForKey:@"CFBundleVersion"];
    if (![installedBundleVersion isKindOfClass:[NSString class]] || [installedBundleVersion length] == 0) {
        return YES;
    }

    NSBundle* appBundle = [NSBundle mainBundle];
    NSURL* appBundleURL = [appBundle bundleURL];
    NSURL* currentHelperToolURL =
        [appBundleURL URLByAppendingPathComponent:
         @"Contents/Library/LaunchServices/com.zgilburd.yubiswitch.helper"];
    NSDictionary* currentInfoPlist =
        (NSDictionary*)CFBridgingRelease(CFBundleCopyInfoDictionaryForURL((CFURLRef)currentHelperToolURL));
    NSString* currentBundleVersion = [currentInfoPlist objectForKey:@"CFBundleVersion"];
    if (![currentBundleVersion isKindOfClass:[NSString class]] || [currentBundleVersion length] == 0) {
        return YES;
    }

    NSLog(@"helper installedVersion: %@", installedBundleVersion);
    NSLog(@"helper currentVersion: %@", currentBundleVersion);
    if (![currentBundleVersion isEqualToString:installedBundleVersion]) {
        return YES;
    }

    // Verify the LaunchDaemon plist is owned by root. A stale plist with wrong
    // ownership will cause launchctl bootstrap to fail with an I/O error.
    NSString *plistPath = [NSString stringWithFormat:@"/Library/LaunchDaemons/%@.plist", label];
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:plistPath error:nil];
    if (attrs && [[attrs fileOwnerAccountName] isEqualToString:@"root"] == NO) {
        NSLog(@"LaunchDaemon plist has wrong ownership, re-blessing helper");
        return YES;
    }

    return NO;
}

- (BOOL)blessHelperWithLabel:(NSString *)label error:(NSError **)error {

    BOOL result = NO;

    AuthorizationItem authItem = {kSMRightBlessPrivilegedHelper, 0, NULL, 0};
    AuthorizationRights authRights = {1, &authItem};
    AuthorizationFlags flags =
    kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed |
    kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights;
    AuthorizationRef authRef = NULL;

    /* Obtain the right to install privileged helper tools
     * (kSMRightBlessPrivilegedHelper). */
    OSStatus status = AuthorizationCreate(
                                          &authRights, kAuthorizationEmptyEnvironment, flags, &authRef);
    if (status != errAuthorizationSuccess) {
        NSLog(@"Failed to bless helper");
    } else {
        // SMJobBless validates both signatures before replacing the installed
        // helper. Preserve the current LaunchDaemon if validation fails.
        result = SMJobBless(kSMDomainSystemLaunchd, (__bridge CFStringRef)label,
                            authRef, (void *)error);
    }

    if (authRef != NULL) AuthorizationFree(authRef, kAuthorizationFlagDefaults);

    return result;
}

- (void)raiseAlertWindow:(NSString *)message {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert addButtonWithTitle:@"OK"];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:message];
    [alert runModal];
}

- (void)notificationReloadHandler:(NSNotification *)notification {
    if ([[notification name] isEqualToString:@"changeDefaultsPrefs"]) {
        NSMutableDictionary *preferences = (NSMutableDictionary *)[notification object];
        NSString *vendor = preferences[@"hotKeyVendorID"];
        NSString *product = preferences[@"hotKeyProductID"];
        if ([vendor isEqualToString:selectedVendorID] &&
            [product isEqualToString:selectedProductID] &&
            removalManager != NULL) {
            preferences[@"applySucceeded"] = @YES;
            return;
        }
        NSString *oldVendor = selectedVendorID;
        NSString *oldProduct = selectedProductID;
        BOOL changedHelper = NO;
        // Unknown physical state does not imply a request to keep the key off.
        // KeyOn clears the helper's disabled policy even when the key is absent
        // or the restore has not completed yet.
        if (requestedDisabled) {
            if (![self action:@"disable" vendorID:vendor productID:product]) {
                preferences[@"applySucceeded"] = @NO;
                return;
            }
            changedHelper = ![vendor isEqualToString:oldVendor] ||
                ![product isEqualToString:oldProduct];
        }
        if (![self registerKeyRemovalWithVendorID:vendor productID:product]) {
            if (changedHelper && oldVendor != nil && oldProduct != nil) {
                if (![self action:@"disable" vendorID:oldVendor
                            productID:oldProduct]) {
                    // A failed rollback can leave either filter disabled.
                    // Restore every saved attachment before retrying the old
                    // policy, then make any unresolved state visible.
                    BOOL restored = [self action:@"enable" vendorID:vendor
                                           productID:product];
                    BOOL oldDisabled = restored &&
                        [self action:@"disable" vendorID:oldVendor
                                    productID:oldProduct];
                    if (!oldDisabled) {
                        stateKnown = NO;
                        selectedVendorID = nil;
                        selectedProductID = nil;
                        [self raiseAlertWindow:@"The YubiKey filter could not be restored. Its state is unknown. Check the key connection and use the menu to enable it before trying again."];
                    }
                }
            } else if (changedHelper) {
                // No previous filter exists, so clear the new helper policy.
                if (![self action:@"enable" vendorID:vendor productID:product]) {
                    stateKnown = NO;
                    selectedVendorID = nil;
                    selectedProductID = nil;
                    [self raiseAlertWindow:@"The YubiKey state could not be restored. Check the key connection and use the menu to enable it before trying again."];
                }
            }
            preferences[@"applySucceeded"] = @NO;
            return;
        }
        preferences[@"applySucceeded"] = @YES;
    }
}

static BOOL parseHexID(NSString *text, unsigned int *value) {
    if (![text isKindOfClass:[NSString class]]) return NO;
    NSScanner *scanner = [NSScanner scannerWithString:text];
    *value = 0;
    return [scanner scanHexInt:value] && [scanner isAtEnd] &&
           *value > 0 && *value <= UINT16_MAX;
}

- (BOOL)action:(NSString *)action {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    return [self action:action
              vendorID:[defaults stringForKey:@"hotKeyVendorID"]
             productID:[defaults stringForKey:@"hotKeyProductID"]];
}

- (BOOL)action:(NSString *)action vendorID:(NSString *)vendor
       productID:(NSString *)product {
    BOOL enabling = [action isEqualToString:@"enable"];
    if (!enabling && ![action isEqualToString:@"disable"]) return NO;
    if (enabling) lockOnRemoval = YES;
    unsigned int idVendor = 0;
    unsigned int idProduct = 0;
    if (!parseHexID(vendor, &idVendor) ||
        !parseHexID(product, &idProduct)) {
        stateKnown = NO;
        NSLog(@"Invalid YubiKey vendor or product filter");
        return NO;
    }
    requestedDisabled = !enabling;
    if (helperConnection == NULL) {
        helperConnection = xpc_connection_create_mach_service(
            "com.zgilburd.yubiswitch.helper", NULL,
            XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
        if (helperConnection == NULL) {
            [self raiseAlertWindow:@"Failed to create XPC connection with helper"];
            stateKnown = NO;
            return NO;
        }
        helperConnectionGeneration++;
        uint64_t connectionGeneration = helperConnectionGeneration;
        __weak YubiKey *weakSelf = self;
        xpc_connection_set_event_handler(helperConnection, ^(xpc_object_t event) {
            if (xpc_get_type(event) != XPC_TYPE_ERROR) return;
            YubiKey *key = weakSelf;
            if (key == nil) return;
            BOOL invalid = event == XPC_ERROR_CONNECTION_INVALID;
            uint_fast64_t completedGeneration = atomic_load_explicit(
                &key->completedActionGeneration, memory_order_relaxed);
            dispatch_async(dispatch_get_main_queue(), ^{
                YubiKey *currentKey = weakSelf;
                if (currentKey == nil ||
                    currentKey->helperConnectionGeneration != connectionGeneration ||
                    atomic_load_explicit(&currentKey->completedActionGeneration,
                                         memory_order_relaxed) != completedGeneration) return;
                currentKey->stateKnown = NO;
                if (invalid) {
                    xpc_connection_cancel(currentKey->helperConnection);
                    currentKey->helperConnection = NULL;
                }
            });
            if (event == XPC_ERROR_CONNECTION_INTERRUPTED) {
                NSLog(@"XPC connection interrupted.");
            } else if (event == XPC_ERROR_CONNECTION_INVALID) {
                NSLog(@"XPC connection invalid.");
            } else if (event == XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT) {
                NSLog(@"Helper rejected the app signing identity.");
            } else {
                NSLog(@"Unexpected XPC connection error.");
            }
        });
        xpc_connection_resume(helperConnection);
    }

    changingState = YES;
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_int64(message, "idVendor", idVendor);
    xpc_dictionary_set_int64(message, "idProduct", idProduct);
    xpc_dictionary_set_int64(message, "request", enabling ? 1 : 0);
    xpc_object_t event = xpc_connection_send_message_with_reply_sync(helperConnection, message);
    const char *response = xpc_get_type(event) == XPC_TYPE_DICTIONARY ?
        xpc_dictionary_get_string(event, "reply") : NULL;
    BOOL present = response != NULL && strcmp(response, "OK") == 0;
    BOOL absent = enabling && response != NULL &&
        strcmp(response, "ABSENT") == 0;
    BOOL succeeded = present || absent;
    stateKnown = present;
    atomic_fetch_add_explicit(&completedActionGeneration, 1,
                              memory_order_relaxed);
    if (succeeded) {
        suspend = !enabling;
        lockOnRemoval = enabling;
    }
    changingState = NO;
    if (event == XPC_ERROR_CONNECTION_INVALID) {
        xpc_connection_cancel(helperConnection);
        helperConnection = NULL;
    }
    return succeeded;
    // NSAppleScript *lockScript = [[NSAppleScript alloc]
    // initWithSource:@"activate application \"ScreenSaverEngine\""];
    // [lockScript executeAndReturnError:nil];
}

- (BOOL)state {
    return suspend;
}
- (BOOL)isStateKnown {
    return stateKnown;
}
- (BOOL)isChangingState {
    return changingState;
}
- (BOOL)enable {
    return [self action:@"enable"];
}

- (BOOL)disable {
    return [self action:@"disable"];
}

- (void)dealloc {
    if (helperConnection != NULL) xpc_connection_cancel(helperConnection);
}


// deal with disconnection of device from usb port

static void handle_removal_callback(void *context, IOReturn result,
                                    void *sender, IOHIDDeviceRef device) {
    YubiKey *key = (__bridge YubiKey *)context;
    if (sender != key->removalManager || [key isChangingState]) return;
    if (!key->suspend) key->stateKnown = NO;
    if (!key->lockOnRemoval) return;
    if ([[NSUserDefaults standardUserDefaults] boolForKey:@"lockWhenUnplugged"]) {
        NSLog(@"YubiKey removed, locking computer");
        NSAppleScript *lockScript =
        [[NSAppleScript alloc]
         initWithSource:@"tell application \"System Events\" to tell current screen saver to start"];
        [lockScript executeAndReturnError:nil];
    }
}

static void match_set(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

- (BOOL)registerKeyRemoval {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    return [self registerKeyRemovalWithVendorID:[defaults stringForKey:@"hotKeyVendorID"]
                                       productID:[defaults stringForKey:@"hotKeyProductID"]];
}

- (BOOL)registerKeyRemovalWithVendorID:(NSString *)vendor
                               productID:(NSString *)product {

    unsigned int idVendor = 0;
    unsigned int idProduct = 0;

    if (!parseHexID(vendor, &idVendor) || !parseHexID(product, &idProduct)) return NO;
    IOHIDManagerRef candidate = IOHIDManagerCreate(kCFAllocatorDefault,
                                                   kIOHIDOptionsTypeNone);
    if (candidate == NULL) return NO;

    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                                             0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    if (match == NULL) {
        CFRelease(candidate);
        return NO;
    }
    match_set(match, CFSTR(kIOHIDVendorIDKey), idVendor);
    match_set(match, CFSTR(kIOHIDProductIDKey), idProduct);
    match_set(match, CFSTR(kIOHIDDeviceUsagePageKey), 1);
    match_set(match, CFSTR(kIOHIDDeviceUsageKey), 6);

    IOHIDManagerScheduleWithRunLoop(candidate, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOHIDManagerSetDeviceMatching(candidate, match);
    IOHIDManagerRegisterDeviceRemovalCallback(candidate, handle_removal_callback,
                                               (__bridge void *)self);

    CFRelease(match);
    IOReturn opened = IOHIDManagerOpen(candidate, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        NSLog(@"Could not open YubiKey removal matcher: 0x%x", opened);
        IOHIDManagerUnscheduleFromRunLoop(candidate, CFRunLoopGetMain(),
                                          kCFRunLoopCommonModes);
        CFRelease(candidate);
        return NO;
    }
    if (removalManager != NULL) {
        IOHIDManagerUnscheduleFromRunLoop(removalManager, CFRunLoopGetMain(),
                                          kCFRunLoopCommonModes);
        IOHIDManagerClose(removalManager, kIOHIDOptionsTypeNone);
        CFRelease(removalManager);
    }
    removalManager = candidate;
    selectedVendorID = [vendor copy];
    selectedProductID = [product copy];
    return YES;
}

@end
