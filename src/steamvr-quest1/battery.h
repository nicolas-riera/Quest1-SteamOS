// Battery status of the headset and the Touch controllers for driver_quest1 (SteamVR properties
// DeviceProvidesBatteryStatus / DeviceBatteryPercentage / DeviceIsCharging, which Steam's VR UI shows
// in the dashboard bar and the quick access device list).
#pragma once

#include <openvr_driver.h>

enum Quest1BatteryDevice
{
	Quest1Battery_Head = 0,
	Quest1Battery_Left,
	Quest1Battery_Right,
	Quest1Battery_Count
};

//! Registers a device's property container (from its Activate) and starts the poll thread if needed.
//! vr::k_ulInvalidPropertyContainer detaches it (Deactivate).
void Quest1BatteryAttach(Quest1BatteryDevice which, vr::PropertyContainerHandle_t container);

//! Stops the poll thread (provider Cleanup).
void Quest1BatteryShutdown();
