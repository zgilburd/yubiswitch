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

#include <syslog.h>
#include <os/log.h>
#include <xpc/xpc.h>

#define ylog(fmt, ...) os_log(OS_LOG_DEFAULT, fmt, ##__VA_ARGS__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <signal.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Authorization.h>


IOHIDManagerRef hidManager;
IOHIDDeviceRef hidDevice;
IOUSBDeviceInterface **usbDevice;
Boolean usbDeviceDeconfigured;
UInt8 savedConfiguration;

static void match_set(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

// Get IOUSBDeviceInterface for the given VID/PID.
static IOUSBDeviceInterface **usb_device_get(int vendorID, int productID) {
    CFMutableDictionaryRef matchDict = IOServiceMatching("IOUSBHostDevice");
    if (!matchDict) {
        ylog("Failed to create USB matching dictionary");
        return NULL;
    }

    CFNumberRef vidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendorID);
    CFNumberRef pidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &productID);
    CFDictionarySetValue(matchDict, CFSTR("idVendor"), vidRef);
    CFDictionarySetValue(matchDict, CFSTR("idProduct"), pidRef);
    CFRelease(vidRef);
    CFRelease(pidRef);

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matchDict);
    if (!service) {
        ylog("USB device not found");
        return NULL;
    }

    IOCFPlugInInterface **plugIn = NULL;
    SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugIn, &score);
    IOObjectRelease(service);

    if (kr != kIOReturnSuccess || !plugIn) {
        ylog("Failed to create USB plugin interface: 0x%x", kr);
        return NULL;
    }

    IOUSBDeviceInterface **dev = NULL;
    (*plugIn)->QueryInterface(plugIn,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
        (LPVOID *)&dev);
    (*plugIn)->Release(plugIn);
    return dev;
}

// Deconfigure/reconfigure the USB device. Setting configuration to 0
// releases all interfaces (HID, FIDO, CCID), making the device inert —
// the LED and capacitive touch sensor won't respond.
static void usb_device_deconfigure(int vendorID, int productID, Boolean deconfigure) {
    if (deconfigure && usbDeviceDeconfigured) return;
    if (!deconfigure && !usbDeviceDeconfigured) return;

    if (!deconfigure && usbDevice != NULL) {
        // Restore the original configuration
        ylog("Restoring USB configuration %d", savedConfiguration);
        IOReturn r = (*usbDevice)->SetConfiguration(usbDevice, savedConfiguration);
        if (r == kIOReturnSuccess) {
            ylog("USB device reconfigured (restored)");
        } else {
            ylog("Failed to restore USB config: 0x%x", r);
            // Try a USB reset as fallback to fully re-enumerate
            (*usbDevice)->ResetDevice(usbDevice);
            ylog("USB device reset issued");
        }
        (*usbDevice)->USBDeviceClose(usbDevice);
        (*usbDevice)->Release(usbDevice);
        usbDevice = NULL;
        usbDeviceDeconfigured = false;
        return;
    }

    usbDevice = usb_device_get(vendorID, productID);
    if (!usbDevice) return;

    IOReturn r = (*usbDevice)->USBDeviceOpenSeize(usbDevice);
    if (r != kIOReturnSuccess) {
        ylog("Failed to open/seize USB device: 0x%x, trying regular open", r);
        r = (*usbDevice)->USBDeviceOpen(usbDevice);
        if (r != kIOReturnSuccess) {
            ylog("Failed to open USB device: 0x%x", r);
            (*usbDevice)->Release(usbDevice);
            usbDevice = NULL;
            return;
        }
    }

    // Save current configuration
    r = (*usbDevice)->GetConfiguration(usbDevice, &savedConfiguration);
    if (r != kIOReturnSuccess) {
        ylog("Failed to get current config: 0x%x, assuming 1", r);
        savedConfiguration = 1;
    }
    ylog("Current USB configuration: %d", savedConfiguration);

    // Set configuration to 0 (unconfigured)
    r = (*usbDevice)->SetConfiguration(usbDevice, 0);
    if (r == kIOReturnSuccess) {
        ylog("USB device deconfigured (config set to 0)");
        usbDeviceDeconfigured = true;
    } else {
        ylog("Failed to deconfigure USB device: 0x%x", r);
        // Try USB device suspend as fallback
        r = (*usbDevice)->USBDeviceSuspend(usbDevice, true);
        if (r == kIOReturnSuccess) {
            ylog("USB device suspended (fallback)");
            usbDeviceDeconfigured = true;
        } else {
            ylog("USB suspend fallback also failed: 0x%x", r);
            (*usbDevice)->USBDeviceClose(usbDevice);
            (*usbDevice)->Release(usbDevice);
            usbDevice = NULL;
        }
    }
}

static void handle_removal_callback(void *context, IOReturn result,
                                    void *sender, IOHIDDeviceRef device) {
    if (hidDevice != NULL) {
        ylog( "device unplugged");
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
    }
    if (hidManager != NULL) {
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        hidManager = NULL;
    }
}

static void match_callback(void *context, IOReturn result, void *sender,
                           IOHIDDeviceRef device) {
    IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
    if (r == kIOReturnSuccess) {
        ylog( "Open'ed HID device");
        hidDevice = device;
    } else {
        ylog( "Failed to open HID device, error: %d", r);
    }
}

static CFDictionaryRef matching_dictionary_create(int vendorID, int productID,
                                                  int usagePage, int usage) {
    CFMutableDictionaryRef match =
        CFDictionaryCreateMutable(kCFAllocatorDefault,
                                  0,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);

    if (vendorID) {
        match_set(match, CFSTR(kIOHIDVendorIDKey), vendorID);
    }
    if (productID) {
        match_set(match, CFSTR(kIOHIDProductIDKey), productID);
    }
    if (usagePage) {
        match_set(match, CFSTR(kIOHIDDeviceUsagePageKey), usagePage);
    }
    if (usage) {
        match_set(match, CFSTR(kIOHIDDeviceUsageKey), usage);
    }

    return match;
}

static void __XPC_Peer_Event_Handler(xpc_connection_t connection,
                                     xpc_object_t event) {
    xpc_type_t type = xpc_get_type(event);

    if (type == XPC_TYPE_ERROR) {
        const char *description = xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);
        ylog( "XPC error: %s", description);
    } else {
        uint64_t idProduct = xpc_dictionary_get_int64(event, "idProduct");
        uint64_t idVendor = xpc_dictionary_get_int64(event, "idVendor");
        uint64_t action = xpc_dictionary_get_int64(event, "request");
        ylog(
               "Received message. idProduct: %llu, idVendor: %llu, action: %llu",
               idProduct, idVendor, action);
        if (action == 1) {
            // enable — resume USB device first, then release HID seize
            usb_device_deconfigure((int)idVendor, (int)idProduct, false);
            if (hidDevice != NULL) {
                IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
                hidDevice = NULL;
            }
            if (hidManager != NULL) {
                IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
                hidManager = NULL;
            }
        } else {
            // disable — seize HID first, then suspend USB device
            if (hidManager == NULL) {
                hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
                IOHIDManagerRegisterDeviceMatchingCallback(hidManager, match_callback, NULL);
                IOHIDManagerRegisterDeviceRemovalCallback(hidManager, handle_removal_callback, NULL);
                IOHIDManagerScheduleWithRunLoop(hidManager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
            }
            CFDictionaryRef match = matching_dictionary_create((int)idVendor, (int)idProduct, 1, 6);
            IOHIDManagerSetDeviceMatching(hidManager, match);
            CFRelease(match);
            usb_device_deconfigure((int)idVendor, (int)idProduct, true);
        }
        xpc_connection_t remote = xpc_dictionary_get_remote_connection(event);
        xpc_object_t reply = xpc_dictionary_create_reply(event);
        xpc_dictionary_set_string(reply, "reply", "OK");
        xpc_connection_send_message(remote, reply);
        xpc_release(reply);
    }
}

static void __XPC_Connection_Handler(xpc_connection_t connection) {
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        __XPC_Peer_Event_Handler(connection, event);
    });

    xpc_connection_resume(connection);
}

void signalHandler(int signum) {
    ylog( "Received signal %d. Cleaning up...", signum);
    if (usbDeviceDeconfigured && usbDevice != NULL) {
        (*usbDevice)->SetConfiguration(usbDevice, savedConfiguration);
        (*usbDevice)->USBDeviceClose(usbDevice);
        (*usbDevice)->Release(usbDevice);
        usbDevice = NULL;
        usbDeviceDeconfigured = false;
    }
    if (hidDevice != NULL) {
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
    }
    if (hidManager != NULL) {
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        hidManager = NULL;
    }
}

int main(int argc, const char *argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    xpc_connection_t service = xpc_connection_create_mach_service("com.zgilburd.yubiswitch.helper",
                                                                  dispatch_get_main_queue(),
                                                                  XPC_CONNECTION_MACH_SERVICE_LISTENER);

    if (!service) {
        ylog( "Failed to create service.");
        exit(EXIT_FAILURE);
    }

    ylog( "Configuring connection event handler for helper");
    xpc_connection_set_event_handler(service, ^(xpc_object_t connection) {
        __XPC_Connection_Handler(connection);
    });

    xpc_connection_resume(service);
    CFRunLoopRun();
    dispatch_main();
    return EXIT_SUCCESS;
}
