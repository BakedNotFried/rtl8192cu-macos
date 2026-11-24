#pragma once
#include <IOKit/usb/IOUSBLib.h>
#include <stdbool.h>

typedef IOUSBDeviceInterface500 **Device;
int with_identified_adapter(bool (*run)(Device, void *), void *context);
