//  PreferencesController.m
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

#import "PreferencesController.h"
#import <ServiceManagement/ServiceManagement.h>
#include <stdint.h>
#include <math.h>

static BOOL validHexID(NSString *text) {
  if (![text isKindOfClass:[NSString class]]) return NO;
  unsigned int value = 0;
  NSScanner *scanner = [NSScanner scannerWithString:text];
  return [scanner scanHexInt:&value] && [scanner isAtEnd] &&
         value > 0 && value <= UINT16_MAX;
}

@interface PreferencesController ()

@end

@implementation PreferencesController {
  SRShortcutValidator *_validator;
}

+ (NSTimeInterval)validatedSwitchOffInterval:(id)value {
  double seconds = 0;
  if ([value isKindOfClass:[NSNumber class]]) {
    seconds = [value doubleValue];
  } else if ([value isKindOfClass:[NSString class]]) {
    BOOL parsed = NO;
    for (NSLocale *locale in @[[NSLocale currentLocale],
                              [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]]) {
      NSScanner *scanner = [NSScanner scannerWithString:value];
      scanner.locale = locale;
      if ([scanner scanDouble:&seconds] && [scanner isAtEnd]) {
        parsed = YES;
        break;
      }
    }
    if (!parsed) return 0;
  } else {
    return 0;
  }
  return isfinite(seconds) && seconds > 0 ? seconds : 0;
}

- (void)awakeFromNib {
  [super awakeFromNib];
  controller = [NSUserDefaultsController sharedUserDefaultsController];
  NSString *defaultPrefsFile =
      [[NSBundle mainBundle] pathForResource:@"DefaultPreferences"
                                      ofType:@"plist"];
  NSDictionary *defaultPrefs =
      [NSDictionary dictionaryWithContentsOfFile:defaultPrefsFile];
  [controller setInitialValues:defaultPrefs];
  [controller setAppliesImmediately:FALSE];
  BOOL startAtLogin =
      [[NSUserDefaults standardUserDefaults] boolForKey:@"startAtLogin"];
  if (@available(macOS 13.0, *)) {
    SMAppServiceStatus status = [[SMAppService mainAppService] status];
    startAtLogin = status == SMAppServiceStatusEnabled ||
                   status == SMAppServiceStatusRequiresApproval;
  }
  [buttonOpenAtLogin setState:startAtLogin];
  [self.hotkeyrecorder bind:NSValueBinding
                   toObject:controller
                withKeyPath:@"values.hotkey"
                    options:nil];
}

- (BOOL)addAppAsLoginItem {
  if (@available(macOS 13.0, *)) {
    NSError *error = nil;
    if (![[SMAppService mainAppService] registerAndReturnError:&error]) {
      NSLog(@"Failed to register login item: %@", error);
      return NO;
    }
    return YES;
  }
  return NO;
}

- (BOOL)deleteAppFromLoginItem {
  if (@available(macOS 13.0, *)) {
    NSError *error = nil;
    if (![[SMAppService mainAppService] unregisterAndReturnError:&error]) {
      NSLog(@"Failed to unregister login item: %@", error);
      return NO;
    }
    return YES;
  }
  return NO;
}

- (IBAction)SetDefaultsButton:(id)sender {
  [controller revertToInitialValues:self];
  [buttonOpenAtLogin setState:
      [[[controller values] valueForKey:@"startAtLogin"] boolValue]];
}

- (IBAction)OKButton:(id)sender {
  if (![[self window] makeFirstResponder:nil] || ![controller commitEditing]) return;
  NSDictionary *delay = [[controller values] valueForKey:@"switchOffDelay"];
  if ([[delay valueForKey:@"enabled"] boolValue] &&
      [[self class] validatedSwitchOffInterval:[delay valueForKey:@"interval"]] == 0) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Invalid switch-off delay"];
    [alert setInformativeText:@"Enter a number of seconds greater than zero."];
    [alert runModal];
    return;
  }
  NSString *vendor = [[controller values] valueForKey:@"hotKeyVendorID"];
  NSString *product = [[controller values] valueForKey:@"hotKeyProductID"];
  if (!validHexID(vendor) || !validHexID(product)) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Invalid device filter"];
    [alert setInformativeText:@"Enter hexadecimal Vendor and Product IDs."];
    [alert runModal];
    return;
  }
  NSMutableDictionary *devicePreferences = [@{
    @"hotKeyVendorID": vendor,
    @"hotKeyProductID": product
  } mutableCopy];
  [[NSNotificationCenter defaultCenter]
      postNotificationName:@"changeDefaultsPrefs"
                    object:devicePreferences];
  if (![devicePreferences[@"applySucceeded"] boolValue]) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Could not apply device filter"];
    [alert setInformativeText:@"The key state could not be confirmed. Check its connection and try again."];
    [alert runModal];
    return;
  }
  BOOL state = [[buttonOpenAtLogin selectedCell] state] == NSControlStateValueOn;
  BOOL previousState = [[NSUserDefaults standardUserDefaults]
                         boolForKey:@"startAtLogin"];
  // The controller can also hold startAtLogin after Set Defaults. Keep its
  // saved value aligned with the service until the service change succeeds.
  [[controller values] setValue:@(previousState) forKey:@"startAtLogin"];
  NSDictionary *previousDelay = [[NSUserDefaults standardUserDefaults]
      dictionaryForKey:@"switchOffDelay"];
  [controller save:self];
  NSDictionary *savedDelay = [[NSUserDefaults standardUserDefaults]
      dictionaryForKey:@"switchOffDelay"];
  if (![previousDelay isEqualToDictionary:savedDelay]) {
    [[NSNotificationCenter defaultCenter]
        postNotificationName:@"switchOffDelayChanged" object:nil];
  }
  BOOL registered = previousState;
  if (@available(macOS 13.0, *)) {
    SMAppServiceStatus status = [[SMAppService mainAppService] status];
    registered = status == SMAppServiceStatusEnabled ||
                 status == SMAppServiceStatusRequiresApproval;
  }
  if (state != registered) {
    BOOL changed = state ? [self addAppAsLoginItem] :
                           [self deleteAppFromLoginItem];
    if (!changed) {
      NSAlert *alert = [[NSAlert alloc] init];
      [alert setAlertStyle:NSAlertStyleWarning];
      [alert setMessageText:@"Could not update Open at Login"];
      if (@available(macOS 13.0, *)) {
        [alert setInformativeText:@"The other preferences were saved. Try again to update the login item."];
      } else {
        [alert setInformativeText:@"Open at Login requires macOS 13 or later. The other preferences were saved."];
      }
      [alert runModal];
      return;
    }
  }
  [[NSUserDefaults standardUserDefaults] setBool:state forKey:@"startAtLogin"];
  [[controller values] setValue:@(state) forKey:@"startAtLogin"];
  [controller save:self];
  [[self window] close];
  if (state) {
    if (@available(macOS 13.0, *)) {
      if ([[SMAppService mainAppService] status] ==
          SMAppServiceStatusRequiresApproval) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setAlertStyle:NSAlertStyleInformational];
        [alert setMessageText:@"Approve Open at Login"];
        [alert setInformativeText:@"macOS needs your approval in Login Items before YubiSwitch can open at login."];
        [alert runModal];
      }
    }
  }
}

- (IBAction)CancelButton:(id)sender {
  [controller revert:self];
  [[self window] close];
}

@end
