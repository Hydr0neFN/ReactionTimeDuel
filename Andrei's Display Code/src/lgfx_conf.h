#pragma once

#define LGFX_USE_V1

#include <driver/gpio.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

class LGFX_CONF : public lgfx::LGFX_Device {
public:
    LGFX_CONF();

private:
    lgfx::Bus_RGB   _bus_instance;
    lgfx::Panel_RGB _panel_instance;

    void setPanelInstance();
    void setBusInstance();
};