#ifndef YUBISWITCH_USB_POLICY_H
#define YUBISWITCH_USB_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int vendorID;
    int productID;
    uint64_t registryID;
    uint32_t locationID;
    void *handle;
} USBPolicyDevice;

typedef struct USBPolicyRecord USBPolicyRecord;
typedef struct {
    USBPolicyRecord *records;
} USBPolicy;

typedef struct {
    void *context;
    bool (*list)(void *, int, int, USBPolicyDevice **, size_t *);
    void (*release)(void *, USBPolicyDevice *, size_t);
    bool (*getConfiguration)(void *, const USBPolicyDevice *, unsigned char *);
    bool (*getRecoveryConfiguration)(void *, const USBPolicyDevice *, unsigned char *);
    bool (*setConfiguration)(void *, const USBPolicyDevice *, unsigned char);
    bool (*setSuspended)(void *, const USBPolicyDevice *, bool);
    bool (*isReady)(void *, const USBPolicyDevice *);
} USBPolicyOps;

typedef struct {
    void *context;
    bool (*configure)(void *, int, int);
    void (*close)(void *);
} USBPolicyHIDOps;

bool usb_policy_disable(USBPolicy *policy, const USBPolicyOps *ops,
                        int vendorID, int productID);
bool usb_policy_restore(USBPolicy *policy, const USBPolicyOps *ops,
                        int vendorID, int productID);
bool usb_policy_restore_all(USBPolicy *policy, const USBPolicyOps *ops);
bool usb_policy_restore_except(USBPolicy *policy, const USBPolicyOps *ops,
                               int vendorID, int productID);
bool usb_policy_change_filter(USBPolicy *policy, const USBPolicyOps *ops,
                              const USBPolicyHIDOps *hid, int oldVendor,
                              int oldProduct, int newVendor, int newProduct);
bool usb_policy_has_pending(const USBPolicy *policy);
void usb_policy_clear(USBPolicy *policy);

#endif
