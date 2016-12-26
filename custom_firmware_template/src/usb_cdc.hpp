/*
 * Copyright (C) 2015 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#pragma once

#include <hal.h>
#include <array>
#include <cstdint>

namespace usb_cdc
{

typedef std::array<std::uint8_t, 16> DeviceSerialNumber;

void init(const DeviceSerialNumber& device_serial);

SerialUSBDriver* getSerialUSBDriver();

enum class State
{
    Disconnected,
    Connected
};

State getState();

}
