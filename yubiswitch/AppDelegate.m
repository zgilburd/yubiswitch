//  AppDelegate.m
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

#import "AppDelegate.h"
#import "AboutWindowController.h"
#import <ShortcutRecorder/ShortcutRecorder.h>

@interface AppDelegate ()
- (BOOL)applyInitialKeyPolicy;
- (void)refreshReDisableTimer;
- (void)showInvalidDelayAlert;
@end

// This is the main class, responsible for the status bar icon and general
// application behavior

@implementation AppDelegate
@synthesize window;
@synthesize controller;

- (id)init {
    self = [super init];
    if (self) {
        // Set default values for preferences, load it from
        // DefaultPreferences.plist file
        NSString *defaultPrefsFile =
        [[NSBundle mainBundle] pathForResource:@"DefaultPreferences"
                                        ofType:@"plist"];
        NSDictionary *defaultPrefs =
        [NSDictionary dictionaryWithContentsOfFile:defaultPrefsFile];

        [[NSUserDefaults standardUserDefaults] registerDefaults:defaultPrefs];

        dispatch_async(dispatch_get_main_queue(), ^{
            m_timer =
            [NSTimer scheduledTimerWithTimeInterval:1
                                             target:self
                                           selector:@selector(updateMenuState:)
                                           userInfo:nil
                                            repeats:YES];
        });
    }
    return self;
}

- (void)updateMenuState:(NSTimer *)foo {
    if (yk == nil) return;
    if (![yk isStateKnown]) {
        [self showUnknownState];
        return;
    }
    BOOL enabled = ![yk state];
    if (displayedStateKnown && isEnabled == enabled) return;
    displayedStateKnown = true;
    isEnabled = enabled;
    [statusItem.button setTitle:@""];
    [statusItem.button setToolTip:enabled ? @"YubiKey enabled" : @"YubiKey disabled"];
    [statusItem.button setImage:[NSImage imageNamed:
                                 enabled ? @"YubikeyEnabled" : @"YubikeyDisabled"]];
    [[statusMenu itemAtIndex:0] setState:enabled ? 1 : 0];
    [self notify:enabled ? @"YubiKey enabled" : @"YubiKey disabled"];
}

- (void)showUnknownState {
    displayedStateKnown = false;
    [statusItem.button setImage:nil];
    [statusItem.button setTitle:@"?"];
    [statusItem.button setToolTip:@"YubiKey status unknown"];
    [[statusMenu itemAtIndex:0] setState:NSControlStateValueMixed];
}
- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    yk = [[YubiKey alloc] init];
    state_monitor = [[ComputerStateMonitor alloc] initWithYubiKey:yk];
    BOOL applied = [self applyInitialKeyPolicy];
    [self updateMenuState:nil];
    if (!applied) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setAlertStyle:NSAlertStyleWarning];
        [alert setMessageText:@"Could not set the YubiKey startup state"];
        [alert setInformativeText:@"YubiSwitch could not confirm the key's state. Check its connection, then try again."];
        [alert runModal];
    }
    UNUserNotificationCenter *center = [UNUserNotificationCenter currentNotificationCenter];
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError *error) {
        if (!granted) {
            NSLog(@"Notification permission not granted");
        }
    }];
    aboutwc = [[AboutWindowController alloc]
               initWithWindowNibName:@"AboutWindowController"];
    prefwc = [[PreferencesController alloc]
              initWithWindowNibName:@"PreferencesController"];
}

- (BOOL)applyInitialKeyPolicy {
    BOOL applied = [state_monitor allowsAutomaticDisable] ? [yk disable] : [yk enable];
    if (!applied) [state_monitor scheduleAutomaticRetry];
    return applied;
}

- (void)awakeFromNib {

    statusItem = [[NSStatusBar systemStatusBar]
                  statusItemWithLength:NSVariableStatusItemLength];
    [statusItem setMenu:statusMenu];
    isEnabled = false;
    [self showUnknownState];

    // setup observer method, when the global hotkey is changed in the
    // preference controller this method gets notified.
    NSUserDefaultsController *defaults =
    [NSUserDefaultsController sharedUserDefaultsController];
    NSString *keyPath = @"values.hotkey";
    [defaults addObserver:self
               forKeyPath:keyPath
                  options:NSKeyValueObservingOptionInitial
                  context:NULL];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(switchOffDelayDidChange:)
                                                 name:@"switchOffDelayChanged"
                                               object:nil];

    SRShortcutAction *hotkeyAction =
    [SRShortcutAction shortcutActionWithKeyPath:keyPath
                                       ofObject:defaults
                                  actionHandler:^BOOL(SRShortcutAction *anAction) {
        [self toggle:nil];
        return YES;
    }];
    [[SRGlobalShortcutMonitor sharedMonitor] addAction:hotkeyAction forKeyEvent:SRKeyEventTypeDown];

    controller = [NSUserDefaultsController sharedUserDefaultsController];
    NSString *defaultPrefsFile =
    [[NSBundle mainBundle] pathForResource:@"DefaultPreferences"
                                    ofType:@"plist"];
    NSDictionary *defaultPrefs =
    [NSDictionary dictionaryWithContentsOfFile:defaultPrefsFile];
    [controller setInitialValues:defaultPrefs];
    [controller setAppliesImmediately:NO];
}

- (void)observeValueForKeyPath:(NSString *)aKeyPath
                      ofObject:(id)anObject
                        change:(NSDictionary *)aChange
                       context:(void *)aContext {
    if ([aKeyPath isEqualToString:@"values.hotkey"]) {
        NSDictionary *hotkey =
        [[NSUserDefaults standardUserDefaults] dictionaryForKey:@"hotkey"];
        [[statusMenu itemAtIndex:0]
         setKeyEquivalent:[hotkey valueForKey:@"charactersIgnoringModifiers"]];
        [[statusMenu itemAtIndex:0]
         setKeyEquivalentModifierMask:
         [[hotkey valueForKey:@"modifierFlags"] unsignedIntValue]];
    } else
        [super observeValueForKeyPath:aKeyPath
                             ofObject:anObject
                               change:aChange
                              context:aContext];
}

- (void)notify:(NSString *)msg {
    BOOL displayNotifications = [[NSUserDefaults standardUserDefaults]
                                 boolForKey:@"displayNotifications"];
    if (!displayNotifications)
        return;
    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = msg;
    UNNotificationRequest *request =
        [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString]
                                             content:content
                                             trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:request
        withCompletionHandler:nil];
}

// TODO: create enable/disable methods and use them in toggle: and createTimer:
- (NSTimer *)createTimer:(NSTimeInterval)interval {
    return [NSTimer scheduledTimerWithTimeInterval:interval
                                            target:self
                                          selector:@selector(reDisableYK)
                                          userInfo:nil
                                           repeats:NO];
}

- (void)reDisableYK {
    reDisableTimer = nil;
    NSDictionary *delay = [[NSUserDefaults standardUserDefaults] dictionaryForKey:@"switchOffDelay"];
    if (![[delay valueForKey:@"enabled"] boolValue] ||
        [PreferencesController validatedSwitchOffInterval:[delay valueForKey:@"interval"]] == 0) return;
    if ([yk isStateKnown] && [yk state]) return;
    if (![state_monitor allowsAutomaticDisable]) {
        reDisableTimer = [self createTimer:5];
        return;
    }
    BOOL res;
    res = [yk disable];
    if (res == TRUE && [yk isStateKnown]) {
        displayedStateKnown = true;
        [statusItem.button setTitle:@""];
        [statusItem.button setToolTip:(@"YubiKey disabled")];
        [statusItem.button setImage:[NSImage imageNamed:@"YubikeyDisabled"]];
        isEnabled = false;
        [[statusMenu itemAtIndex:0] setState:0];
        [self notify:@"YubiKey disabled"];
    } else {
        [self showUnknownState];
        reDisableTimer = [self createTimer:10];
    }
}

- (void)enableYubiKey:(BOOL)enable {
    BOOL res;
    if (enable == TRUE) {
        res = [yk enable];
    } else {
        res = [yk disable];
    }
    if (res) [state_monitor cancelAutomaticRetry];
    if (res == TRUE && [yk isStateKnown]) {
        displayedStateKnown = true;
        [statusItem.button setTitle:@""];
        [reDisableTimer invalidate];
        reDisableTimer = nil;
        if (enable == TRUE) {
            [statusItem.button setToolTip:(@"YubiKey enabled")];
            [statusItem.button setImage:[NSImage imageNamed:@"YubikeyEnabled"]];
            isEnabled = true;
            [[statusMenu itemAtIndex:0] setState:1];
            [self notify:@"YubiKey enabled"];
            [self refreshReDisableTimer];
        } else {
            [statusItem.button setToolTip:(@"YubiKey disabled")];
            [statusItem.button setImage:[NSImage imageNamed:@"YubikeyDisabled"]];
            isEnabled = false;
            [[statusMenu itemAtIndex:0] setState:0];
            [self notify:@"YubiKey disabled"];
        }
    } else {
        [self showUnknownState];
        if (enable) {
            NSAlert *alert = [[NSAlert alloc] init];
            [alert setAlertStyle:NSAlertStyleWarning];
            [alert setMessageText:@"Could not fully enable the YubiKey"];
            [alert setInformativeText:@"YubiSwitch could not confirm that the key is ready. Check its connection, then try enabling it again."];
            [alert runModal];
        }
    }
}

-(bool)application:(NSApplication *)sender delegateHandlesKey:(NSString *)key {
    return [key isEqualToString:@"status"] ||
           [key isEqualToString:@"statusKnown"];
}

- (bool)status {
    return [yk isStateKnown] && ![yk state];
}

- (bool)statusKnown {
    return [yk isStateKnown];
}

- (IBAction)toggle:(id)sender {
    if (![yk isStateKnown]) {
        [self enableYubiKey:TRUE];
    } else if (![yk state]) {
        [self enableYubiKey:FALSE];
    } else {
        [self enableYubiKey:TRUE];
    }
}

- (IBAction)toggleSwitchOffDelay:(id)sender {
    NSDictionary *delay = [[controller values] valueForKey:@"switchOffDelay"];
    if ([[delay valueForKey:@"enabled"] boolValue] &&
        [PreferencesController validatedSwitchOffInterval:[delay valueForKey:@"interval"]] == 0) {
        NSMutableDictionary *reverted = [delay mutableCopy];
        reverted[@"enabled"] = @NO;
        [[controller values] setValue:reverted forKey:@"switchOffDelay"];
        [controller save:self];
        [self refreshReDisableTimer];
        [self showInvalidDelayAlert];
        return;
    }
    [controller save:self];
    [self refreshReDisableTimer];
}

- (void)showInvalidDelayAlert {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Invalid switch-off delay"];
    [alert setInformativeText:@"Set a positive number of seconds in Preferences before enabling automatic switch-off."];
    [alert runModal];
}

- (void)switchOffDelayDidChange:(NSNotification *)notification {
    [self refreshReDisableTimer];
}

- (void)refreshReDisableTimer {
    [reDisableTimer invalidate];
    reDisableTimer = nil;
    if (yk == nil || ([yk isStateKnown] && [yk state])) return;
    NSDictionary *delay = [[NSUserDefaults standardUserDefaults] dictionaryForKey:@"switchOffDelay"];
    if (![[delay valueForKey:@"enabled"] boolValue]) return;
    NSTimeInterval interval = [PreferencesController validatedSwitchOffInterval:
                               [delay valueForKey:@"interval"]];
    if (interval > 0) reDisableTimer = [self createTimer:interval];
}

- (IBAction)toggleLockWhenUnplugged:(id)sender {
    [controller save:self];
}

- (IBAction)toggleDisplayNotications:(id)sender {
    [controller save:self];
}

- (IBAction)toggleDisableSleepLock:(id)sender {
    [controller save:self];
}

- (IBAction)about:(id)sender {
    [[aboutwc window] makeKeyAndOrderFront:self];
    [[aboutwc window] setOrderedIndex:0];
    [NSApp activate];
    [aboutwc showWindow:self];
}

- (IBAction)pref:(id)sender {
    [[prefwc window] makeKeyAndOrderFront:self];
    [[prefwc window] setOrderedIndex:0];
    [NSApp activate];
    [prefwc showWindow:self];
}

- (IBAction)quit:(id)sender {
    if (![yk enable]) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setAlertStyle:NSAlertStyleWarning];
        [alert setMessageText:@"Could not restore the YubiKey"];
        [alert setInformativeText:@"YubiSwitch will stay open so you can retry enabling the key before quitting."];
        [alert runModal];
        return;
    }
    [self notify:@"YubiKey enabled"];
    [reDisableTimer invalidate];
    reDisableTimer = nil;
    [[NSApplication sharedApplication] terminate:self];
}

- (void)lockComputer {
    NSAppleScript *lockScript = [[NSAppleScript alloc] initWithSource:@"activate application \"ScreenSaverEngine\""];
    [lockScript executeAndReturnError:nil];
}

@end
