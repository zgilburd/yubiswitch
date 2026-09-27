#ifndef YUBISWITCH_CLIENT_IDENTITY_H
#define YUBISWITCH_CLIENT_IDENTITY_H

// Keep this requirement aligned with SMAuthorizedClients in the helper's
// embedded Info.plist. SMJobBless checks installation, not XPC requests.
#define YUBISWITCH_CLIENT_REQUIREMENT \
    "anchor apple generic and identifier \"com.zgilburd.yubiswitch\" " \
    "and certificate leaf[subject.OU] = \"39YQGAPYYW\""

#endif
