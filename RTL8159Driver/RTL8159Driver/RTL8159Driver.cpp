// RTL8159Driver.cpp
// Personality A: USB config selector
//
// Matches IOUSBHostDevice (VID=0x0BDA, PID=0x815A).
// Opens device, selects Config 1 (vendor-specific, class 255), keeps session
// open to prevent the USB stack from reverting to Config 2 (NCM) or Config 3 (ECM).

#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>

#include "RTL8159Driver.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "RTL8159-A [" __DATE__ " " __TIME__ "] " fmt, ##__VA_ARGS__)

struct RTL8159Driver_IVars {
    IOUSBHostDevice *device;
};

bool
RTL8159Driver::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(RTL8159Driver_IVars, 1);
    return ivars != nullptr;
}

void
RTL8159Driver::free()
{
    IOSafeDeleteNULL(ivars, RTL8159Driver_IVars, 1);
    super::free();
}

kern_return_t
IMPL(RTL8159Driver, Start)
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
    // releasing it causes the stack to revert to Config 2 (NCM).
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
IMPL(RTL8159Driver, Stop)
{
    Log("Stop");
    if (ivars->device != nullptr) {
        ivars->device->Close(this, 0);
        OSSafeReleaseNULL(ivars->device);
    }
    return Stop(provider, SUPERDISPATCH);
}
