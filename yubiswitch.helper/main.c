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
#include <IOKit/usb/IOUSBHostFamilyDefinitions.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Authorization.h>
#include "usb_policy.h"
#include "client_identity.h"


IOHIDManagerRef hidManager;
IOHIDDeviceRef hidDevice;
static USBPolicy usbPolicy;
static Boolean desiredDisabled;
static int selectedVendorID;
static int selectedProductID;
static uint64_t requestGeneration;
static xpc_connection_t controllerConnection;
static IONotificationPortRef usbNotifyPort;
static io_iterator_t usbAddedIterator;

// Power management notification port and notifier
static io_connect_t pmRootPort;
static IONotificationPortRef pmNotifyPort;
static io_object_t pmNotifier;
enum { kUSBReconcileAttempts = 8 };

static void match_set(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

static int64_t usb_property_number(io_service_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key,
                                                       kCFAllocatorDefault, 0);
    int64_t number = 0;
    if (value != NULL) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number);
        }
        CFRelease(value);
    }
    return number;
}

static IOUSBDeviceInterface182 **usb_interface_create(io_service_t service) {
    IOCFPlugInInterface **plugIn = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugIn, &score);
    if (kr != kIOReturnSuccess || plugIn == NULL) {
        ylog("Failed to create USB plugin interface: 0x%x", kr);
        return NULL;
    }

    IOUSBDeviceInterface182 **dev = NULL;
    HRESULT result = (*plugIn)->QueryInterface(plugIn,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), (LPVOID *)&dev);
    (*plugIn)->Release(plugIn);
    if (result != S_OK || dev == NULL) {
        ylog("Failed to obtain USB interface revision 182: 0x%x", (unsigned int)result);
        return NULL;
    }
    return dev;
}

static IOReturn usb_interface_open(IOUSBDeviceInterface182 **dev) {
    IOReturn result = (*dev)->USBDeviceOpenSeize(dev);
    if (result != kIOReturnSuccess) result = (*dev)->USBDeviceOpen(dev);
    return result;
}

static bool usb_list_devices(void *context, int vendorID, int productID,
                             USBPolicyDevice **devices, size_t *count) {
    (void)context;
    *devices = NULL;
    *count = 0;
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (match == NULL) return false;
    match_set(match, CFSTR("idVendor"), vendorID);
    match_set(match, CFSTR("idProduct"), productID);
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match,
                                     &iterator) != KERN_SUCCESS) return false;
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != 0) {
        USBPolicyDevice *grown = realloc(*devices,
                                         (*count + 1) * sizeof(**devices));
        if (grown == NULL) {
            IOObjectRelease(service);
            for (size_t i = 0; i < *count; i++) {
                IOObjectRelease((io_service_t)(uintptr_t)(*devices)[i].handle);
            }
            free(*devices);
            *devices = NULL;
            *count = 0;
            IOObjectRelease(iterator);
            return false;
        }
        *devices = grown;
        uint64_t registryID = 0;
        IORegistryEntryGetRegistryEntryID(service, &registryID);
        (*devices)[(*count)++] = (USBPolicyDevice){
            vendorID, productID, registryID,
            (uint32_t)usb_property_number(service,
                                          CFSTR(kUSBDevicePropertyLocationID)),
            (void *)(uintptr_t)service
        };
    }
    IOObjectRelease(iterator);
    return true;
}

static void usb_release_devices(void *context, USBPolicyDevice *devices,
                                size_t count) {
    (void)context;
    for (size_t i = 0; i < count; i++) {
        IOObjectRelease((io_service_t)(uintptr_t)devices[i].handle);
    }
    free(devices);
}

static bool usb_get_configuration(void *context,
                                  const USBPolicyDevice *device,
                                  unsigned char *configuration) {
    (void)context;
    IOUSBDeviceInterface182 **dev = usb_interface_create(
        (io_service_t)(uintptr_t)device->handle);
    if (dev == NULL) return false;
    UInt8 value = 0;
    IOReturn result = (*dev)->GetConfiguration(dev, &value);
    (*dev)->Release(dev);
    if (result != kIOReturnSuccess) {
        ylog("Could not read USB configuration for registry %llu at location 0x%x: 0x%x",
             (unsigned long long)device->registryID, device->locationID,
             result);
        return false;
    }
    *configuration = value;
    return true;
}

static bool usb_get_recovery_configuration(void *context,
                                           const USBPolicyDevice *device,
                                           unsigned char *configuration) {
    (void)context;
    IOUSBDeviceInterface182 **dev = usb_interface_create(
        (io_service_t)(uintptr_t)device->handle);
    if (dev == NULL) return false;
    UInt8 count = 0;
    IOUSBConfigurationDescriptorPtr descriptor = NULL;
    bool unique = (*dev)->GetNumberOfConfigurations(dev, &count) ==
                      kIOReturnSuccess && count == 1 &&
                  (*dev)->GetConfigurationDescriptorPtr(dev, 0,
                                                        &descriptor) ==
                      kIOReturnSuccess && descriptor != NULL &&
                  descriptor->bConfigurationValue != 0;
    if (unique) {
        *configuration = descriptor->bConfigurationValue;
        ylog("USB recovery selected sole configuration %u for registry %llu at location 0x%x",
             *configuration, (unsigned long long)device->registryID,
             device->locationID);
    }
    (*dev)->Release(dev);
    if (!unique) {
        ylog("Cannot identify a unique USB recovery configuration for registry %llu at location 0x%x",
             (unsigned long long)device->registryID, device->locationID);
    }
    return unique;
}

static bool usb_set_configuration(void *context,
                                  const USBPolicyDevice *device,
                                  unsigned char configuration) {
    (void)context;
    IOUSBDeviceInterface182 **dev = usb_interface_create(
        (io_service_t)(uintptr_t)device->handle);
    if (dev == NULL) return false;
    IOReturn result = usb_interface_open(dev);
    if (result == kIOReturnSuccess) {
        result = (*dev)->SetConfiguration(dev, configuration);
        (*dev)->USBDeviceClose(dev);
    }
    (*dev)->Release(dev);
    if (result != kIOReturnSuccess) {
        ylog("Could not set USB configuration %u for registry %llu at location 0x%x: 0x%x",
             configuration, (unsigned long long)device->registryID,
             device->locationID, result);
    } else {
        ylog("USB configuration %u accepted for registry %llu at location 0x%x; awaiting verification",
             configuration, (unsigned long long)device->registryID,
             device->locationID);
    }
    return result == kIOReturnSuccess;
}

static bool usb_set_suspended(void *context, const USBPolicyDevice *device,
                              bool suspended) {
    (void)context;
    IOUSBDeviceInterface182 **dev = usb_interface_create(
        (io_service_t)(uintptr_t)device->handle);
    if (dev == NULL) return false;
    IOReturn result = usb_interface_open(dev);
    if (result == kIOReturnSuccess) {
        result = (*dev)->USBDeviceSuspend(dev, suspended);
        (*dev)->USBDeviceClose(dev);
    }
    (*dev)->Release(dev);
    if (result != kIOReturnSuccess) {
        ylog("USB %s failed for registry %llu at location 0x%x: 0x%x",
             suspended ? "suspend" : "resume",
             (unsigned long long)device->registryID, device->locationID,
             result);
    } else if (suspended) {
        ylog("USB suspend accepted for registry %llu at location 0x%x",
             (unsigned long long)device->registryID, device->locationID);
    }
    return result == kIOReturnSuccess;
}

static unsigned usb_interface_count(io_registry_entry_t parent, unsigned depth) {
    io_iterator_t iterator = 0;
    if (IORegistryEntryGetChildIterator(parent, kIOServicePlane,
                                         &iterator) != KERN_SUCCESS) return 0;
    unsigned count = 0;
    io_registry_entry_t child;
    while ((child = IOIteratorNext(iterator)) != 0) {
        if (IOObjectConformsTo(child, kIOUSBHostInterfaceClassName) ||
            IOObjectConformsTo(child, "IOUSBInterface")) {
            count++;
        } else if (depth < 3) {
            count += usb_interface_count(child, depth + 1);
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(iterator);
    return count;
}

static unsigned usb_expected_interface_count(const USBPolicyDevice *device) {
    IOUSBDeviceInterface182 **dev = usb_interface_create(
        (io_service_t)(uintptr_t)device->handle);
    if (dev == NULL) return 0;
    UInt8 configuration = 0;
    UInt8 configurations = 0;
    unsigned expected = 0;
    if ((*dev)->GetConfiguration(dev, &configuration) == kIOReturnSuccess &&
        configuration != 0 &&
        (*dev)->GetNumberOfConfigurations(dev, &configurations) ==
            kIOReturnSuccess) {
        for (UInt8 index = 0; index < configurations; index++) {
            IOUSBConfigurationDescriptorPtr descriptor = NULL;
            if ((*dev)->GetConfigurationDescriptorPtr(dev, index,
                                                        &descriptor) ==
                    kIOReturnSuccess && descriptor != NULL &&
                descriptor->bConfigurationValue == configuration) {
                expected = descriptor->bNumInterfaces;
                break;
            }
        }
    }
    (*dev)->Release(dev);
    return expected;
}

static bool usb_is_ready(void *context, const USBPolicyDevice *device) {
    (void)context;
    unsigned expected = usb_expected_interface_count(device);
    if (expected == 0) {
        ylog("Could not determine expected USB interfaces for registry %llu at location 0x%x",
             (unsigned long long)device->registryID, device->locationID);
        return false;
    }
    io_service_t service = (io_service_t)(uintptr_t)device->handle;
    mach_timespec_t quietTime = {2, 0};
    IOReturn quiet = IOServiceWaitQuiet(service, &quietTime);
    if (quiet != kIOReturnSuccess) {
        ylog("USB interface enumeration did not settle for registry %llu at location 0x%x: 0x%x",
             (unsigned long long)device->registryID, device->locationID,
             quiet);
        return false;
    }
    unsigned observed = 0;
    for (unsigned attempt = 0; attempt < 10; attempt++) {
        observed = usb_interface_count(service, 0);
        if (observed >= expected) {
            ylog("USB interfaces ready for registry %llu at location 0x%x: %u/%u",
                 (unsigned long long)device->registryID, device->locationID,
                 observed, expected);
            return true;
        }
        usleep(100000);
    }
    ylog("USB interfaces not ready for registry %llu at location 0x%x: %u/%u",
         (unsigned long long)device->registryID, device->locationID,
         observed, expected);
    return false;
}

static const USBPolicyOps usbOps = {
    NULL, usb_list_devices, usb_release_devices, usb_get_configuration,
    usb_get_recovery_configuration, usb_set_configuration,
    usb_set_suspended, usb_is_ready
};

static Boolean usb_device_enable(void) {
    bool allRestored = usb_policy_restore_all(&usbPolicy, &usbOps);
    bool selectedReady = selectedVendorID == 0 ||
        usb_policy_restore(&usbPolicy, &usbOps,
                           selectedVendorID, selectedProductID);
    return allRestored && selectedReady;
}

static bool selected_device_present(bool *present) {
    USBPolicyDevice *devices = NULL;
    size_t count = 0;
    if (!usb_list_devices(NULL, selectedVendorID, selectedProductID,
                          &devices, &count)) return false;
    *present = count != 0;
    usb_release_devices(NULL, devices, count);
    return true;
}

static Boolean usb_device_disable(int vendorID, int productID) {
    return usb_policy_disable(&usbPolicy, &usbOps, vendorID, productID);
}

static void handle_removal_callback(void *context, IOReturn result,
                                    void *sender, IOHIDDeviceRef device) {
    if (hidDevice == device) {
        ylog( "device unplugged");
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
    }
}

static void match_callback(void *context, IOReturn result, void *sender,
                           IOHIDDeviceRef device) {
    if (!desiredDisabled || sender != hidManager) return;
    IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
    if (r == kIOReturnSuccess) {
        ylog( "Open'ed HID device");
        hidDevice = device;
    } else {
        ylog( "Failed to open HID device, error: %d", r);
    }
}

static void close_hid_manager(void) {
    if (hidDevice != NULL) {
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
    }
    if (hidManager != NULL) {
        IOHIDManagerUnscheduleFromRunLoop(hidManager, CFRunLoopGetMain(),
                                          kCFRunLoopCommonModes);
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        CFRelease(hidManager);
        hidManager = NULL;
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

static Boolean configure_hid_manager(int vendorID, int productID) {
    if (hidManager == NULL) {
        hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        if (hidManager == NULL) return false;
        IOHIDManagerRegisterDeviceMatchingCallback(hidManager,
                                                   match_callback, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(hidManager,
                                                  handle_removal_callback, NULL);
        IOHIDManagerScheduleWithRunLoop(hidManager, CFRunLoopGetMain(),
                                        kCFRunLoopCommonModes);
        CFDictionaryRef match = matching_dictionary_create(vendorID, productID,
                                                            1, 6);
        IOHIDManagerSetDeviceMatching(hidManager, match);
        CFRelease(match);
        IOReturn result = IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone);
        if (result != kIOReturnSuccess) {
            ylog("Could not open HID manager: 0x%x", result);
            close_hid_manager();
            return false;
        }
    }
    return true;
}

static bool policy_configure_hid(void *context, int vendorID, int productID) {
    (void)context;
    return configure_hid_manager(vendorID, productID);
}

static void policy_close_hid(void *context) {
    (void)context;
    close_hid_manager();
}

static const USBPolicyHIDOps hidOps = {
    NULL, policy_configure_hid, policy_close_hid
};

static void schedule_usb_reconcile(unsigned attempts, uint64_t generation) {
    if (attempts == 0) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (generation != requestGeneration) {
            ylog("USB reconciliation cancelled by a newer request (generation %llu)",
                 (unsigned long long)generation);
            return;
        }
        bool success;
        if (desiredDisabled) {
            bool restoredOthers = usb_policy_restore_except(
                &usbPolicy, &usbOps, selectedVendorID, selectedProductID);
            bool hidReady = configure_hid_manager(selectedVendorID,
                                                  selectedProductID);
            bool usbReady = hidReady && usb_device_disable(
                selectedVendorID, selectedProductID);
            success = restoredOthers && hidReady && usbReady;
        } else {
            success = usb_device_enable();
        }
        if (success) {
            ylog("USB reconciliation completed after attempt %u; desired disabled=%d, pending restore records=%d",
                 kUSBReconcileAttempts - attempts + 1, desiredDisabled,
                 usb_policy_has_pending(&usbPolicy));
        } else if (attempts == 1) {
            ylog("USB reconciliation exhausted %u attempts; desired disabled=%d, pending restore records=%d",
                 kUSBReconcileAttempts, desiredDisabled,
                 usb_policy_has_pending(&usbPolicy));
        } else {
            schedule_usb_reconcile(attempts - 1, generation);
        }
    });
}

static void usb_matched_callback(void *context, io_iterator_t iterator) {
    io_service_t service;
    unsigned matched = 0;
    while ((service = IOIteratorNext(iterator)) != 0) {
        matched++;
        IOObjectRelease(service);
    }
    if (desiredDisabled || usb_policy_has_pending(&usbPolicy)) {
        ylog("USB match notification (%u devices); reconciling disabled=%d, pending restore records=%d",
             matched, desiredDisabled, usb_policy_has_pending(&usbPolicy));
        schedule_usb_reconcile(kUSBReconcileAttempts, requestGeneration);
    }
}

static void controller_disconnected(xpc_connection_t connection) {
    if (controllerConnection != connection) return;
    ylog("App connection lost; restoring USB policy");
    xpc_release(controllerConnection);
    controllerConnection = NULL;
    requestGeneration++;
    desiredDisabled = false;
    close_hid_manager();
    if (!usb_device_enable()) {
        ylog("App connection lost before USB restore completed");
        schedule_usb_reconcile(kUSBReconcileAttempts, requestGeneration);
    } else {
        ylog("USB restore check completed after app disconnect; pending restore records=%d",
             usb_policy_has_pending(&usbPolicy));
    }
}

static void __XPC_Peer_Event_Handler(xpc_connection_t connection,
                                     xpc_object_t event) {
    xpc_type_t type = xpc_get_type(event);

    if (type == XPC_TYPE_ERROR) {
        if (event == XPC_ERROR_CONNECTION_INVALID ||
            event == XPC_ERROR_CONNECTION_INTERRUPTED) {
            controller_disconnected(connection);
        }
        const char *description = xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);
        ylog( "XPC error: %s", description);
        return;
    }
    if (type != XPC_TYPE_DICTIONARY) return;

    int64_t vendor = xpc_dictionary_get_int64(event, "idVendor");
    int64_t product = xpc_dictionary_get_int64(event, "idProduct");
    int64_t action = xpc_dictionary_get_int64(event, "request");
    Boolean valid = (action == 0 || action == 1) &&
                    vendor > 0 && vendor <= UINT16_MAX &&
                    product > 0 && product <= UINT16_MAX;
    if (valid && controllerConnection == NULL) {
        controllerConnection = xpc_retain(connection);
    }
    if (controllerConnection != connection) valid = false;
    if (valid) {
        ylog("USB policy request: %s for %04x:%04x (generation %llu)",
             action == 1 ? "enable" : "disable", (unsigned int)vendor,
             (unsigned int)product,
             (unsigned long long)(requestGeneration + 1));
    } else {
        ylog("Rejected invalid or non-controller USB policy request");
    }
    Boolean success = false;
    bool selectedPresent = true;
    if (valid && action == 1) {
        requestGeneration++;
        selectedVendorID = (int)vendor;
        selectedProductID = (int)product;
        desiredDisabled = false;
        close_hid_manager();
        success = usb_device_enable();
        if (success && !selected_device_present(&selectedPresent)) {
            success = false;
        }
        if (!success) schedule_usb_reconcile(kUSBReconcileAttempts, requestGeneration);
    } else if (valid) {
        int oldVendor = selectedVendorID;
        int oldProduct = selectedProductID;
        Boolean filterChanged = desiredDisabled &&
            (oldVendor != vendor || oldProduct != product);
        if (filterChanged) {
            ylog("Changing disabled USB filter from %04x:%04x to %04x:%04x",
                 (unsigned int)oldVendor, (unsigned int)oldProduct,
                 (unsigned int)vendor, (unsigned int)product);
            success = usb_policy_change_filter(
                &usbPolicy, &usbOps, &hidOps, oldVendor, oldProduct,
                (int)vendor, (int)product);
            if (success) {
                selectedVendorID = (int)vendor;
                selectedProductID = (int)product;
            } else {
                ylog("USB filter change failed; previous filter remains requested, pending restore records=%d",
                     usb_policy_has_pending(&usbPolicy));
            }
        } else {
            Boolean wasDisabled = desiredDisabled;
            selectedVendorID = (int)vendor;
            selectedProductID = (int)product;
            desiredDisabled = true;
            Boolean restoredOthers = usb_policy_restore_except(
                &usbPolicy, &usbOps, selectedVendorID, selectedProductID);
            Boolean hidReady = configure_hid_manager(selectedVendorID,
                                                      selectedProductID);
            Boolean usbReady = hidReady && usb_device_disable(
                selectedVendorID, selectedProductID);
            success = restoredOthers && hidReady && usbReady;
            if (!success && !wasDisabled) {
                ylog("First USB disable failed; restoring the previously enabled policy");
                // A failed first disable can already have changed USB state.
                // Restore it and keep the previously enabled policy.
                desiredDisabled = false;
                close_hid_manager();
                selectedVendorID = oldVendor;
                selectedProductID = oldProduct;
                if (!usb_device_enable()) {
                    ylog("Failed disable left USB restore work pending");
                } else {
                    ylog("Failed disable rollback check completed; pending restore records=%d",
                         usb_policy_has_pending(&usbPolicy));
                }
            }
        }
        requestGeneration++;
        if (!success) schedule_usb_reconcile(kUSBReconcileAttempts, requestGeneration);
    }
    if (valid) {
        ylog("USB policy result: %s for %04x:%04x; desired disabled=%d, pending restore records=%d",
             !success ? "ERROR" : !selectedPresent ? "ABSENT" : "OK",
             (unsigned int)vendor, (unsigned int)product, desiredDisabled,
             usb_policy_has_pending(&usbPolicy));
    }
    xpc_object_t reply = xpc_dictionary_create_reply(event);
    if (reply != NULL) {
        xpc_dictionary_set_string(reply, "reply", !success ? "ERROR" :
                                  !selectedPresent ? "ABSENT" : "OK");
        xpc_connection_t remote = xpc_dictionary_get_remote_connection(event);
        xpc_connection_send_message(remote, reply);
        xpc_release(reply);
    }
}

static void __XPC_Connection_Handler(xpc_connection_t connection) {
    // USB policy and IOKit callbacks share state on the main queue.
    xpc_connection_set_target_queue(connection, dispatch_get_main_queue());
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        __XPC_Peer_Event_Handler(connection, event);
    });

    xpc_connection_resume(connection);
}

static void powerCallback(void *refCon, io_service_t service,
                          natural_t messageType, void *messageArgument) {
    switch (messageType) {
        case kIOMessageSystemWillSleep:
            IOAllowPowerChange(pmRootPort, (long)messageArgument);
            break;
        case kIOMessageCanSystemSleep:
            IOAllowPowerChange(pmRootPort, (long)messageArgument);
            break;
        case kIOMessageSystemHasPoweredOn:
            if (desiredDisabled || usb_policy_has_pending(&usbPolicy)) {
                ylog("Wake detected; reconciling disabled=%d, pending restore records=%d",
                     desiredDisabled, usb_policy_has_pending(&usbPolicy));
                schedule_usb_reconcile(kUSBReconcileAttempts, requestGeneration);
            }
            break;
        default:
            break;
    }
}

int main(int argc, const char *argv[]) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
                                                     SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_t interrupt = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
                                                          SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{
        ylog("SIGTERM received; restoring USB before helper exit");
        close_hid_manager();
        Boolean restored = usb_device_enable();
        if (!restored) ylog("Helper exited before USB restore completed");
        else ylog("USB restore check completed before helper exit; pending restore records=%d",
                  usb_policy_has_pending(&usbPolicy));
        exit(restored ? EXIT_SUCCESS : EXIT_FAILURE);
    });
    dispatch_source_set_event_handler(interrupt, ^{
        ylog("SIGINT received; restoring USB before helper exit");
        close_hid_manager();
        Boolean restored = usb_device_enable();
        if (!restored) ylog("Helper exited before USB restore completed");
        else ylog("USB restore check completed before helper exit; pending restore records=%d",
                  usb_policy_has_pending(&usbPolicy));
        exit(restored ? EXIT_SUCCESS : EXIT_FAILURE);
    });
    dispatch_resume(term);
    dispatch_resume(interrupt);

    // Register for system sleep/wake notifications
    pmRootPort = IORegisterForSystemPower(NULL, &pmNotifyPort,
                                          powerCallback, &pmNotifier);
    if (pmRootPort && pmNotifyPort != NULL &&
        IONotificationPortGetRunLoopSource(pmNotifyPort) != NULL) {
        CFRunLoopAddSource(CFRunLoopGetMain(),
                           IONotificationPortGetRunLoopSource(pmNotifyPort),
                           kCFRunLoopCommonModes);
        ylog("Registered for system power notifications");
    } else {
        ylog("Could not register system power notifications");
        exit(EXIT_FAILURE);
    }

    usbNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (usbNotifyPort != NULL &&
        IONotificationPortGetRunLoopSource(usbNotifyPort) != NULL) {
        CFRunLoopAddSource(CFRunLoopGetMain(),
                           IONotificationPortGetRunLoopSource(usbNotifyPort),
                           kCFRunLoopCommonModes);
        kern_return_t result = IOServiceAddMatchingNotification(
            usbNotifyPort, kIOFirstMatchNotification,
            IOServiceMatching("IOUSBHostDevice"), usb_matched_callback,
            NULL, &usbAddedIterator);
        if (result == KERN_SUCCESS) {
            usb_matched_callback(NULL, usbAddedIterator);
        } else {
            ylog("Could not register USB match notification: 0x%x", result);
            exit(EXIT_FAILURE);
        }
    } else {
        ylog("Could not create USB match notification port");
        exit(EXIT_FAILURE);
    }

    xpc_connection_t service = xpc_connection_create_mach_service("com.zgilburd.yubiswitch.helper",
                                                                  dispatch_get_main_queue(),
                                                                  XPC_CONNECTION_MACH_SERVICE_LISTENER);

    if (!service) {
        ylog( "Failed to create service.");
        exit(EXIT_FAILURE);
    }

    if (xpc_connection_set_peer_code_signing_requirement(
            service, YUBISWITCH_CLIENT_REQUIREMENT) != 0) {
        ylog("Could not enforce XPC client signing requirement");
        xpc_connection_cancel(service);
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
