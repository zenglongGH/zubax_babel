/*
 * Copyright (C) 2015  Zubax Robotics  <info@zubax.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

#include "board.hpp"
#include <cstring>
#include <ch.hpp>
#include <hal.h>
#include <unistd.h>

/// PAL setup
const ::PALConfig pal_default_config =
{
#if STM32_HAS_GPIOA
    {VAL_GPIOA_MODER,VAL_GPIOA_OTYPER,VAL_GPIOA_OSPEEDR,VAL_GPIOA_PUPDR,VAL_GPIOA_ODR,VAL_GPIOA_AFRL,VAL_GPIOA_AFRH},
#endif
#if STM32_HAS_GPIOB
    {VAL_GPIOB_MODER,VAL_GPIOB_OTYPER,VAL_GPIOB_OSPEEDR,VAL_GPIOB_PUPDR,VAL_GPIOB_ODR,VAL_GPIOB_AFRL,VAL_GPIOB_AFRH},
#endif
#if STM32_HAS_GPIOC
    {VAL_GPIOC_MODER,VAL_GPIOC_OTYPER,VAL_GPIOC_OSPEEDR,VAL_GPIOC_PUPDR,VAL_GPIOC_ODR,VAL_GPIOC_AFRL,VAL_GPIOC_AFRH},
#endif
#if STM32_HAS_GPIOD
    {VAL_GPIOD_MODER,VAL_GPIOD_OTYPER,VAL_GPIOD_OSPEEDR,VAL_GPIOD_PUPDR,VAL_GPIOD_ODR,VAL_GPIOD_AFRL,VAL_GPIOD_AFRH},
#endif
#if STM32_HAS_GPIOE
    {VAL_GPIOE_MODER,VAL_GPIOE_OTYPER,VAL_GPIOE_OSPEEDR,VAL_GPIOE_PUPDR,VAL_GPIOE_ODR,VAL_GPIOE_AFRL,VAL_GPIOE_AFRH},
#endif
#if STM32_HAS_GPIOF
    {VAL_GPIOF_MODER,VAL_GPIOF_OTYPER,VAL_GPIOF_OSPEEDR,VAL_GPIOF_PUPDR,VAL_GPIOF_ODR,VAL_GPIOF_AFRL,VAL_GPIOF_AFRH}
#endif
};

/// Provided by linker
const extern std::uint8_t DeviceSignatureStorage[];

namespace board
{

os::watchdog::Timer init(unsigned watchdog_timeout_msec, os::config::Param<unsigned>& cfg_uart_baudrate)
{
    /*
     * OS initialization first
     */
    halInit();
    chSysInit();

    /*
     * Watchdog
     */
    os::watchdog::init();
    os::watchdog::Timer wdt;
    wdt.startMSec(watchdog_timeout_msec);

    /*
     * Configuration manager
     */
    const int config_init_res = os::config::init();
    if (config_init_res < 0)
    {
        die();
    }

    /*
     * Serial port
     */
    reconfigureUART(cfg_uart_baudrate.get());

    /*
     * Prompt
     */
    os::lowsyslog(PRODUCT_NAME_STRING " %d.%d.%08x / %d %s\n",
                  FW_VERSION_MAJOR, FW_VERSION_MINOR, GIT_HASH, config_init_res,
                  watchdogTriggeredLastReset() ? "WDTRESET" : "OK");
    return wdt;
}

void reconfigureUART(const unsigned baudrate)
{
    static SerialConfig config =
    {
        0,
        0,
        USART_CR2_STOP1_BITS,
        0
    };

    if (baudrate != config.speed)
    {
        config.speed = baudrate;
        sdStart(&STDOUT_SD, &config);
    }
}

__attribute__((noreturn))
void die()
{
    while (1)
    {
        setStatusLED(false);
        setTrafficLED(false);
        ::usleep(25000);
        setStatusLED(true);
        setTrafficLED(true);
        ::usleep(25000);
    }
}

void restart()
{
    NVIC_SystemReset();
}

void setStatusLED(bool state)
{
    palWritePad(GPIOB, GPIOB_LED_STATUS, !state);
}

void setTrafficLED(bool state)
{
    palWritePad(GPIOB, GPIOB_LED_TRAFFIC, !state);
}

void enableCANPower(bool state)
{
    palWritePad(GPIOB, GPIOB_CAN_POWER_DIS, !state);
}

void enableCANTerminator(bool state)
{
    palWritePad(GPIOB, GPIOB_CAN_TERMINATOR_EN, state);
}

UniqueID readUniqueID()
{
    UniqueID bytes;
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(0x1FFFF7AC), std::tuple_size<UniqueID>::value);
    return bytes;
}

bool tryReadDeviceSignature(DeviceSignature& out_sign)
{
    std::memcpy(out_sign.data(), &DeviceSignatureStorage[0], std::tuple_size<DeviceSignature>::value);

    bool valid = false;
    for (auto x : out_sign)
    {
        if (x != 0xFF && x != 0x00)          // All 0xFF/0x00 is not a valid signature, it's empty storage
        {
            valid = true;
            break;
        }
    }

    return valid;
}

HardwareVersion detectHardwareVersion()
{
    auto v = HardwareVersion();

    v.major = HW_VERSION;
    v.minor = 0;                // Some detection will be added in future versions

    return v;
}

} // namespace board

namespace os
{

void applicationHaltHook()
{
    board::setStatusLED(true);
    board::setTrafficLED(true);
}

}

/*
 * Early init from ChibiOS
 */
extern "C"
{

void __early_init(void)
{
    stm32_clock_init();
}

void boardInit(void)
{
}

}
