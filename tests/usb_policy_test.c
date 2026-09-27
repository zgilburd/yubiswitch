#include "../yubiswitch.helper/usb_policy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    USBPolicyDevice devices[4];
    unsigned char configurations[4];
    unsigned char uniqueConfigurations[4];
    bool suspended[4];
    bool ready[4];
    size_t count;
    bool failSuspend;
    bool failSuspendAfterEffect;
    bool readRequiresResume;
    bool descriptorRequiresResume;
    bool failResumeAfterEffect;
    bool failHIDForNewFilter;
    bool failVerifyAfterNewDisable;
    bool verifyFailurePending;
    int suspendCalls;
    int resumeCalls;
    int disableCalls;
    int restoreCalls;
    int hidVendor;
    int hidProduct;
} Fake;

static bool list_devices(void *context, int vendor, int product,
                         USBPolicyDevice **devices, size_t *count) {
    Fake *fake = context;
    *devices = calloc(fake->count, sizeof(**devices));
    if (fake->count && !*devices) return false;
    *count = 0;
    for (size_t i = 0; i < fake->count; i++) {
        if (fake->devices[i].vendorID == vendor &&
            fake->devices[i].productID == product) {
            (*devices)[(*count)++] = fake->devices[i];
        }
    }
    return true;
}

static void release_devices(void *context, USBPolicyDevice *devices,
                            size_t count) {
    (void)context;
    (void)count;
    free(devices);
}

static size_t index_for(Fake *fake, const USBPolicyDevice *device) {
    for (size_t i = 0; i < fake->count; i++) {
        if (device->registryID != 0 &&
            fake->devices[i].registryID == device->registryID) return i;
        if (device->locationID != 0 &&
            fake->devices[i].locationID == device->locationID) return i;
    }
    assert(false);
    return 0;
}

static bool get_configuration(void *context, const USBPolicyDevice *device,
                              unsigned char *configuration) {
    Fake *fake = context;
    if (fake->readRequiresResume &&
        fake->suspended[index_for(fake, device)]) return false;
    if (fake->verifyFailurePending && device->vendorID == 0x2222) {
        fake->verifyFailurePending = false;
        return false;
    }
    *configuration = fake->configurations[index_for(fake, device)];
    return true;
}

static bool get_recovery_configuration(void *context,
                                       const USBPolicyDevice *device,
                                       unsigned char *configuration) {
    Fake *fake = context;
    if (fake->descriptorRequiresResume &&
        fake->suspended[index_for(fake, device)]) return false;
    *configuration = fake->uniqueConfigurations[index_for(fake, device)];
    return *configuration != 0;
}

static bool set_configuration(void *context, const USBPolicyDevice *device,
                              unsigned char configuration) {
    Fake *fake = context;
    fake->configurations[index_for(fake, device)] = configuration;
    if (configuration == 0) fake->disableCalls++;
    else fake->restoreCalls++;
    if (configuration == 0 && device->vendorID == 0x2222 &&
        fake->failVerifyAfterNewDisable) {
        fake->verifyFailurePending = true;
    }
    return true;
}

static bool set_suspended(void *context, const USBPolicyDevice *device,
                          bool suspended) {
    Fake *fake = context;
    size_t index = index_for(fake, device);
    if (suspended) {
        fake->suspendCalls++;
        if (fake->failSuspend) return false;
        if (fake->failSuspendAfterEffect) {
            fake->suspended[index] = true;
            return false;
        }
    } else {
        fake->resumeCalls++;
        if (fake->failResumeAfterEffect) {
            fake->suspended[index] = false;
            return false;
        }
    }
    fake->suspended[index] = suspended;
    return true;
}

static bool is_ready(void *context, const USBPolicyDevice *device) {
    Fake *fake = context;
    size_t index = index_for(fake, device);
    return fake->ready[index] && !fake->suspended[index] &&
           fake->configurations[index] != 0;
}

static bool configure_hid(void *context, int vendor, int product) {
    Fake *fake = context;
    if (fake->failHIDForNewFilter && vendor == 0x2222) return false;
    fake->hidVendor = vendor;
    fake->hidProduct = product;
    return true;
}

static void close_hid(void *context) {
    Fake *fake = context;
    fake->hidVendor = 0;
    fake->hidProduct = 0;
}

static USBPolicyOps operations(Fake *fake) {
    return (USBPolicyOps){fake, list_devices, release_devices,
                          get_configuration, get_recovery_configuration,
                          set_configuration,
                          set_suspended, is_ready};
}

static USBPolicyHIDOps hid_operations(Fake *fake) {
    return (USBPolicyHIDOps){fake, configure_hid, close_hid};
}

static void add_device(Fake *fake, int vendor, int product, uint64_t registry,
                       uint32_t location) {
    size_t index = fake->count++;
    fake->devices[index] = (USBPolicyDevice){vendor, product, registry,
                                              location, NULL};
    fake->configurations[index] = 1;
    fake->uniqueConfigurations[index] = 1;
    fake->ready[index] = true;
}

static void test_failed_suspend_is_resumed_for_uncertain_effect(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.failSuspend = true;
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 0);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.resumeCalls == 1);
    assert(fake.configurations[0] == 1);
    assert(!usb_policy_has_pending(&policy));
    usb_policy_clear(&policy);
}

static void test_suspend_error_after_effect_is_resumed(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.failSuspendAfterEffect = true;
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(fake.suspended[0]);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.resumeCalls == 1);
    assert(!fake.suspended[0]);
    assert(fake.configurations[0] == 1);
    usb_policy_clear(&policy);
}

static void test_resume_error_after_effect_keeps_restored_key_on(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.failResumeAfterEffect = true;
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.resumeCalls == 1);
    assert(fake.configurations[0] == 1);
    assert(fake.disableCalls == 1);
    usb_policy_clear(&policy);
}

static void test_port_move_restores_new_attachment(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    fake.devices[0].registryID = 12;
    fake.devices[0].locationID = 200;
    fake.configurations[0] = 1;
    fake.suspended[0] = false;
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 0);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 1);
    usb_policy_clear(&policy);
}

static void test_two_identical_keys_are_both_restored(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    add_device(&fake, 0x1050, 0x0407, 12, 200);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 0 && fake.configurations[1] == 0);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 1 && fake.configurations[1] == 1);
    usb_policy_clear(&policy);
}

static void test_disconnected_key_keeps_restore_record(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    fake.count = 0;
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(usb_policy_has_pending(&policy));
    usb_policy_clear(&policy);
}

static void test_helper_restart_restores_unique_configuration(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.configurations[0] = 0;
    fake.suspended[0] = true;
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.resumeCalls == 1);
    assert(fake.configurations[0] == 1);
    assert(!fake.suspended[0]);
    usb_policy_clear(&policy);
}

static void test_helper_restart_resumes_before_configuration_read(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.configurations[0] = 0;
    fake.suspended[0] = true;
    fake.readRequiresResume = true;
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 1);
    assert(!fake.suspended[0]);
    usb_policy_clear(&policy);
}

static void test_helper_restart_resumes_before_descriptor_read(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.configurations[0] = 0;
    fake.suspended[0] = true;
    fake.descriptorRequiresResume = true;
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 1);
    assert(!fake.suspended[0]);
    usb_policy_clear(&policy);
}

static void test_helper_restart_rejects_ambiguous_configuration(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    fake.configurations[0] = 0;
    fake.uniqueConfigurations[0] = 0;
    USBPolicyOps ops = operations(&fake);
    assert(!usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 0);
    assert(fake.restoreCalls == 0);
    usb_policy_clear(&policy);
}

static void test_controller_loss_restores_every_filter(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1111, 0x0001, 11, 100);
    add_device(&fake, 0x2222, 0x0002, 12, 200);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1111, 0x0001));
    assert(usb_policy_disable(&policy, &ops, 0x2222, 0x0002));
    assert(usb_policy_has_pending(&policy));
    assert(usb_policy_restore_all(&policy, &ops));
    assert(fake.configurations[0] == 1);
    assert(fake.configurations[1] == 1);
    assert(!usb_policy_has_pending(&policy));
    usb_policy_clear(&policy);
}

static void test_identity_without_registry_id_uses_location(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 0, 100);
    add_device(&fake, 0x1050, 0x0407, 0, 200);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 1 && fake.configurations[1] == 1);
    usb_policy_clear(&policy);
}

static void test_replacement_at_same_port_does_not_get_old_configuration(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    // The original key is unplugged. A different matching key uses the port.
    fake.devices[0].registryID = 12;
    fake.configurations[0] = 2;
    fake.uniqueConfigurations[0] = 2;
    fake.suspended[0] = false;
    int restoresBefore = fake.restoreCalls;
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 2);
    assert(fake.restoreCalls == restoresBefore);
    assert(!usb_policy_has_pending(&policy));
    usb_policy_clear(&policy);
}

static void test_replacement_disabled_at_same_port_retires_old_record(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    fake.devices[0].registryID = 12;
    fake.configurations[0] = 2;
    fake.uniqueConfigurations[0] = 2;
    fake.suspended[0] = false;
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 2);
    assert(!usb_policy_has_pending(&policy));
    usb_policy_clear(&policy);
}

static void test_filter_change_rolls_back_when_hid_setup_fails(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1111, 0x0001, 11, 100);
    add_device(&fake, 0x2222, 0x0002, 12, 200);
    USBPolicyOps ops = operations(&fake);
    USBPolicyHIDOps hid = hid_operations(&fake);
    assert(configure_hid(&fake, 0x1111, 0x0001));
    assert(usb_policy_disable(&policy, &ops, 0x1111, 0x0001));
    fake.failHIDForNewFilter = true;
    assert(!usb_policy_change_filter(&policy, &ops, &hid,
                                     0x1111, 0x0001, 0x2222, 0x0002));
    assert(fake.configurations[0] == 0);
    assert(fake.configurations[1] == 1);
    assert(fake.hidVendor == 0x1111);
    usb_policy_clear(&policy);
}

static void test_not_ready_does_not_report_restored(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1050, 0x0407, 11, 100);
    USBPolicyOps ops = operations(&fake);
    assert(usb_policy_disable(&policy, &ops, 0x1050, 0x0407));
    fake.ready[0] = false;
    assert(!usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    assert(fake.configurations[0] == 1);
    assert(usb_policy_has_pending(&policy));
    fake.ready[0] = true;
    assert(usb_policy_restore(&policy, &ops, 0x1050, 0x0407));
    usb_policy_clear(&policy);
}

static void test_filter_change_restores_partial_new_disable(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1111, 0x0001, 11, 100);
    add_device(&fake, 0x2222, 0x0002, 12, 200);
    USBPolicyOps ops = operations(&fake);
    USBPolicyHIDOps hid = hid_operations(&fake);
    assert(configure_hid(&fake, 0x1111, 0x0001));
    assert(usb_policy_disable(&policy, &ops, 0x1111, 0x0001));
    fake.failVerifyAfterNewDisable = true;
    assert(!usb_policy_change_filter(&policy, &ops, &hid,
                                     0x1111, 0x0001, 0x2222, 0x0002));
    assert(fake.configurations[0] == 0);
    assert(fake.configurations[1] == 1);
    assert(fake.hidVendor == 0x1111);
    usb_policy_clear(&policy);
}

static void test_filter_change_reapplies_old_disable_after_unready_restore(void) {
    Fake fake = {0};
    USBPolicy policy = {0};
    add_device(&fake, 0x1111, 0x0001, 11, 100);
    add_device(&fake, 0x2222, 0x0002, 12, 200);
    USBPolicyOps ops = operations(&fake);
    USBPolicyHIDOps hid = hid_operations(&fake);
    assert(configure_hid(&fake, 0x1111, 0x0001));
    assert(usb_policy_disable(&policy, &ops, 0x1111, 0x0001));
    fake.ready[0] = false;
    assert(!usb_policy_change_filter(&policy, &ops, &hid,
                                     0x1111, 0x0001, 0x2222, 0x0002));
    assert(fake.configurations[0] == 0);
    assert(fake.configurations[1] == 1);
    assert(fake.hidVendor == 0x1111);
    usb_policy_clear(&policy);
}

int main(void) {
    test_failed_suspend_is_resumed_for_uncertain_effect();
    test_suspend_error_after_effect_is_resumed();
    test_resume_error_after_effect_keeps_restored_key_on();
    test_port_move_restores_new_attachment();
    test_two_identical_keys_are_both_restored();
    test_disconnected_key_keeps_restore_record();
    test_helper_restart_restores_unique_configuration();
    test_helper_restart_resumes_before_configuration_read();
    test_helper_restart_resumes_before_descriptor_read();
    test_helper_restart_rejects_ambiguous_configuration();
    test_controller_loss_restores_every_filter();
    test_identity_without_registry_id_uses_location();
    test_replacement_at_same_port_does_not_get_old_configuration();
    test_replacement_disabled_at_same_port_retires_old_record();
    test_filter_change_rolls_back_when_hid_setup_fails();
    test_not_ready_does_not_report_restored();
    test_filter_change_restores_partial_new_disable();
    test_filter_change_reapplies_old_disable_after_unready_restore();
    puts("usb_policy_test: 18 passed");
    return 0;
}
