// RTL8153Driver.cpp
// Personality A: USB config selector
//
// Matches IOUSBHostDevice (VID=0x0BDA, PID=0x8153).
// Opens device, selects Config 1 (vendor-specific, class 255), keeps session
// open to prevent the USB stack from reverting to Config 2 (CDC).

#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>

#include "RTL8153Driver.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "RTL8153-A [" __DATE__ " " __TIME__ "] " fmt, ##__VA_ARGS__)

struct RTL8153Driver_IVars {
    IOUSBHostDevice *device;
};

bool
RTL8153Driver::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(RTL8153Driver_IVars, 1);
    return ivars != nullptr;
}

void
RTL8153Driver::free()
{
    IOSafeDeleteNULL(ivars, RTL8153Driver_IVars, 1);
    super::free();
}

kern_return_t
IMPL(RTL8153Driver, Start)
{
    kern_return_t ret;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("super Start failed: 0x%x", ret);
        return ret;
    }

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (ivars->device == nullptr) {
        Log("provider is not IOUSBHostDevice");
        return kIOReturnError;
    }

    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        Log("device Open failed: 0x%x", ret);
        return ret;
    }

    // Select Config 1 and publish interface nubs. Keep session open —
    // releasing it causes the stack to revert to Config 2 (CDC).
    ret = ivars->device->SetConfiguration(1, true);
    if (ret != kIOReturnSuccess) {
        Log("SetConfiguration(1) failed: 0x%x", ret);
        return ret;
    }
    Log("SetConfiguration(1) -> success");

    ret = RegisterService();
    Log("RegisterService -> 0x%x", ret);
    return ret;
}

kern_return_t
IMPL(RTL8153Driver, Stop)
{
    Log("Stop");
    // Close provider BEFORE SUPERDISPATCH — mirrors AQC111 pattern.
    // Stop_Impl's async cancel completion dereferences a field via the Default
    // queue proxy; closing the device after SUPERDISPATCH races that teardown.
    if (ivars->device != nullptr) {
        ivars->device->Close(this, 0);
        OSSafeReleaseNULL(ivars->device);
    }
    return Stop(provider, SUPERDISPATCH);
}
