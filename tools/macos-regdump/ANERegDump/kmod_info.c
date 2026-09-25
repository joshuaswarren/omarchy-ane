/* kmod_info for ANERegDump, the object Xcode's kext template generates.
 * _start/_stop come from libkmodc++, which runs the C++ constructors and
 * then _realmain (none here). Bundle ID and version match Info.plist. */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.warren.ANERegDump, "1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
