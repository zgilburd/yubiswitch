#include "usb_policy.h"

#include <stdlib.h>

struct USBPolicyRecord {
    int vendorID;
    int productID;
    uint64_t registryID;
    uint32_t locationID;
    unsigned char configuration;
    bool resumeNeeded;
    struct USBPolicyRecord *next;
};

typedef struct {
    int vendorID;
    int productID;
} DeviceFilter;

static bool same_device(const USBPolicyRecord *record,
                        const USBPolicyDevice *device) {
    if (record->vendorID != device->vendorID ||
        record->productID != device->productID) return false;
    if (record->registryID != 0 && device->registryID != 0)
        return record->registryID == device->registryID;
    return record->locationID != 0 &&
           record->locationID == device->locationID;
}

static USBPolicyRecord **find_record(USBPolicy *policy,
                                     const USBPolicyDevice *device) {
    USBPolicyRecord **slot = &policy->records;
    while (*slot != NULL && !same_device(*slot, device)) {
        slot = &(*slot)->next;
    }
    return slot;
}

static USBPolicyRecord *save_record(USBPolicy *policy,
                                    const USBPolicyDevice *device,
                                    unsigned char configuration) {
    if (device->registryID == 0 && device->locationID == 0) return NULL;
    USBPolicyRecord **slot = find_record(policy, device);
    if (*slot == NULL) {
        *slot = calloc(1, sizeof(**slot));
        if (*slot == NULL) return NULL;
    }
    USBPolicyRecord *record = *slot;
    record->vendorID = device->vendorID;
    record->productID = device->productID;
    record->registryID = device->registryID;
    record->locationID = device->locationID;
    record->configuration = configuration;
    record->resumeNeeded = false;
    return record;
}

static void remove_record(USBPolicyRecord **slot) {
    USBPolicyRecord *record = *slot;
    *slot = record->next;
    free(record);
}

static void retire_replaced_record(USBPolicy *policy,
                                   const USBPolicyDevice *device) {
    if (device->registryID == 0 || device->locationID == 0) return;
    USBPolicyRecord **slot = &policy->records;
    while (*slot != NULL) {
        USBPolicyRecord *record = *slot;
        if (record->vendorID == device->vendorID &&
            record->productID == device->productID &&
            record->locationID == device->locationID &&
            record->registryID != 0 &&
            record->registryID != device->registryID) {
            remove_record(slot);
        } else {
            slot = &record->next;
        }
    }
}

bool usb_policy_disable(USBPolicy *policy, const USBPolicyOps *ops,
                        int vendorID, int productID) {
    USBPolicyDevice *devices = NULL;
    size_t count = 0;
    if (!ops->list(ops->context, vendorID, productID, &devices, &count)) {
        return false;
    }
    bool success = count != 0;
    for (size_t i = 0; i < count; i++) {
        const USBPolicyDevice *device = &devices[i];
        unsigned char configuration = 0;
        if (!ops->getConfiguration(ops->context, device, &configuration)) {
            success = false;
            continue;
        }
        USBPolicyRecord **slot = find_record(policy, device);
        if (configuration == 0) {
            if (*slot == NULL) success = false;
            continue;
        }
        USBPolicyRecord *record = save_record(policy, device, configuration);
        if (record == NULL) {
            success = false;
            continue;
        }
        // Keep the restore record even if SetConfiguration reports an error:
        // the device may already have accepted the change.
        ops->setConfiguration(ops->context, device, 0);
        unsigned char verified = 0;
        if (!ops->getConfiguration(ops->context, device, &verified) ||
            verified != 0) {
            success = false;
            continue;
        }
        // A failed call may have taken effect. Resume on every restore after
        // a suspend attempt, then verify configuration and interfaces.
        ops->setSuspended(ops->context, device, true);
        record->resumeNeeded = true;
    }
    ops->release(ops->context, devices, count);
    return success;
}

bool usb_policy_restore(USBPolicy *policy, const USBPolicyOps *ops,
                        int vendorID, int productID) {
    USBPolicyDevice *devices = NULL;
    size_t count = 0;
    if (!ops->list(ops->context, vendorID, productID, &devices, &count)) {
        return false;
    }
    bool success = true;
    for (size_t i = 0; i < count; i++) {
        const USBPolicyDevice *device = &devices[i];
        USBPolicyRecord **slot = find_record(policy, device);
        USBPolicyRecord *record = *slot;
        if (record != NULL && record->resumeNeeded) {
            // An error can mean the resume already took effect. Confirm the
            // observable state below instead of switching the key off again.
            ops->setSuspended(ops->context, device, false);
        }
        unsigned char configuration = 0;
        bool readConfiguration = ops->getConfiguration(
            ops->context, device, &configuration);
        if (!readConfiguration && record == NULL) {
            // A restarted helper has no record that the previous instance
            // suspended this device. Resume before retrying the read.
            ops->setSuspended(ops->context, device, false);
            readConfiguration = ops->getConfiguration(
                ops->context, device, &configuration);
        }
        if (!readConfiguration) {
            success = false;
            continue;
        }
        if (record == NULL && configuration == 0) {
            // Resume before reading descriptors: a previous helper may have
            // suspended the device and then exited without a restore record.
            ops->setSuspended(ops->context, device, false);
            unsigned char recoveryConfiguration = 0;
            if (ops->getRecoveryConfiguration == NULL ||
                !ops->getRecoveryConfiguration(ops->context, device,
                                               &recoveryConfiguration) ||
                recoveryConfiguration == 0) {
                success = false;
                continue;
            }
            // Verify the result below because resume or set can report an
            // error after taking effect.
            ops->setConfiguration(ops->context, device,
                                  recoveryConfiguration);
            if (!ops->getConfiguration(ops->context, device,
                                       &configuration) ||
                configuration != recoveryConfiguration) {
                success = false;
                continue;
            }
        }
        if (record != NULL && configuration != record->configuration) {
            ops->setConfiguration(ops->context, device,
                                  record->configuration);
            if (!ops->getConfiguration(ops->context, device,
                                       &configuration)) {
                success = false;
                continue;
            }
        }
        if (configuration == 0 ||
            (record != NULL && configuration != record->configuration) ||
            !ops->isReady(ops->context, device)) {
            success = false;
            continue;
        }
        if (record != NULL) remove_record(slot);
        retire_replaced_record(policy, device);
    }
    ops->release(ops->context, devices, count);
    return success;
}

static bool restore_matching_filters(USBPolicy *policy,
                                     const USBPolicyOps *ops,
                                     int excludedVendor, int excludedProduct,
                                     bool exclude) {
    DeviceFilter *filters = NULL;
    size_t count = 0;
    for (USBPolicyRecord *record = policy->records; record != NULL;
         record = record->next) {
        if (exclude && record->vendorID == excludedVendor &&
            record->productID == excludedProduct) continue;
        bool found = false;
        for (size_t i = 0; i < count; i++) {
            if (filters[i].vendorID == record->vendorID &&
                filters[i].productID == record->productID) {
                found = true;
                break;
            }
        }
        if (found) continue;
        DeviceFilter *grown = realloc(filters, (count + 1) * sizeof(*filters));
        if (grown == NULL) {
            free(filters);
            return false;
        }
        filters = grown;
        filters[count++] = (DeviceFilter){record->vendorID,
                                           record->productID};
    }
    bool success = true;
    for (size_t i = 0; i < count; i++) {
        if (!usb_policy_restore(policy, ops, filters[i].vendorID,
                                filters[i].productID)) success = false;
    }
    free(filters);
    return success;
}

bool usb_policy_restore_all(USBPolicy *policy, const USBPolicyOps *ops) {
    return restore_matching_filters(policy, ops, 0, 0, false);
}

bool usb_policy_restore_except(USBPolicy *policy, const USBPolicyOps *ops,
                               int vendorID, int productID) {
    return restore_matching_filters(policy, ops, vendorID, productID, true);
}

bool usb_policy_change_filter(USBPolicy *policy, const USBPolicyOps *ops,
                              const USBPolicyHIDOps *hid, int oldVendor,
                              int oldProduct, int newVendor, int newProduct) {
    hid->close(hid->context);
    if (!usb_policy_restore(policy, ops, oldVendor, oldProduct)) {
        hid->configure(hid->context, oldVendor, oldProduct);
        usb_policy_disable(policy, ops, oldVendor, oldProduct);
        return false;
    }
    if (hid->configure(hid->context, newVendor, newProduct) &&
        usb_policy_disable(policy, ops, newVendor, newProduct)) {
        return true;
    }
    // Both filters may have restore records after a partial USB operation.
    // Restore the candidate before reapplying the previous disabled policy.
    usb_policy_restore(policy, ops, newVendor, newProduct);
    hid->close(hid->context);
    hid->configure(hid->context, oldVendor, oldProduct);
    usb_policy_disable(policy, ops, oldVendor, oldProduct);
    return false;
}

bool usb_policy_has_pending(const USBPolicy *policy) {
    return policy->records != NULL;
}

void usb_policy_clear(USBPolicy *policy) {
    while (policy->records != NULL) remove_record(&policy->records);
}
