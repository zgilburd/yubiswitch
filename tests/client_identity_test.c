#include "../yubiswitch.helper/client_identity.h"

#include <Security/Security.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    SecRequirementRef requirement = NULL;
    assert(SecRequirementCreateWithString(
               CFSTR(YUBISWITCH_CLIENT_REQUIREMENT), kSecCSDefaultFlags,
               &requirement) == errSecSuccess);
    assert(requirement != NULL);
    CFRelease(requirement);

    FILE *file = fopen(argv[1], "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    UInt8 *bytes = malloc((size_t)size);
    assert(bytes != NULL);
    assert(fread(bytes, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, size);
    free(bytes);
    assert(data != NULL);
    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    assert(plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID());
    CFArrayRef clients = CFDictionaryGetValue(plist, CFSTR("SMAuthorizedClients"));
    assert(clients != NULL && CFGetTypeID(clients) == CFArrayGetTypeID());
    assert(CFArrayGetCount(clients) == 1);
    assert(CFEqual(CFArrayGetValueAtIndex(clients, 0),
                   CFSTR(YUBISWITCH_CLIENT_REQUIREMENT)));
    CFRelease(plist);
    puts("client_identity_test: requirement parsed and matches helper plist");
    return 0;
}
