//  YubiKey.h
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

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Authorization.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <xpc/xpc.h>
#include <stdatomic.h>
#include <stdint.h>


@interface YubiKey : NSObject {
    BOOL suspend;
    BOOL requestedDisabled;
    BOOL stateKnown;
    BOOL changingState;
    BOOL lockOnRemoval;
    NSString *selectedVendorID;
    NSString *selectedProductID;
    IOHIDManagerRef removalManager;
    xpc_connection_t helperConnection;
    uint64_t helperConnectionGeneration;
    atomic_uint_fast64_t completedActionGeneration;
}

-(id)init;
-(BOOL)action:(NSString *)action;
-(BOOL)state;
-(BOOL)isStateKnown;
-(BOOL)isChangingState;
-(BOOL)enable;
-(BOOL)disable;
-(void)notificationReloadHandler:(NSNotification *)notification;
-(BOOL)blessHelperWithLabel:(NSString *)label error:(NSError **)error;
-(BOOL)needToInstallHelper:(NSString *)label;


@end
