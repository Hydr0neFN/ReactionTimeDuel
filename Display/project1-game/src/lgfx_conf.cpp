#include "lgfx_conf.h"

LGFX_CONF::LGFX_CONF() {
    setPanelInstance();      // <-- do panel first
    setBusInstance();        // <-- then bus
    setPanel(&_panel_instance);
}

void LGFX_CONF::setPanelInstance() {
    auto cfg = _panel_instance.config();

    cfg.panel_width  = 800;
    cfg.panel_height = 480;

    cfg.memory_width  = 800;
    cfg.memory_height = 480;

    cfg.offset_x = 0;
    cfg.offset_y = 0;

    cfg.bus_shared = false;
    cfg.invert = false;
    cfg.rgb_order = true; // flip if colors are swapped
    _panel_instance.config(cfg);
}

void LGFX_CONF::setBusInstance() {
     auto bus_cfg = _bus_instance.config();
    bus_cfg.panel = &_panel_instance;

    // Control signals
    bus_cfg.pin_henable = GPIO_NUM_5;   // DE
    bus_cfg.pin_pclk    = GPIO_NUM_7;
    bus_cfg.pin_vsync   = GPIO_NUM_3;
    bus_cfg.pin_hsync   = GPIO_NUM_46;

    // RGB565 data: D0..D4=B3..B7, D5..D10=G2..G7, D11..D15=R3..R7
    bus_cfg.pin_d0  = GPIO_NUM_14; // B3
    bus_cfg.pin_d1  = GPIO_NUM_38; // B4
    bus_cfg.pin_d2  = GPIO_NUM_18; // B5
    bus_cfg.pin_d3  = GPIO_NUM_17; // B6
    bus_cfg.pin_d4  = GPIO_NUM_10; // B7

    bus_cfg.pin_d5  = GPIO_NUM_39; // G2
    bus_cfg.pin_d6  = GPIO_NUM_0;  // G3
    bus_cfg.pin_d7  = GPIO_NUM_45; // G4
    bus_cfg.pin_d8  = GPIO_NUM_48; // G5
    bus_cfg.pin_d9  = GPIO_NUM_47; // G6
    bus_cfg.pin_d10 = GPIO_NUM_21; // G7

    bus_cfg.pin_d11 = GPIO_NUM_1;  // R3
    bus_cfg.pin_d12 = GPIO_NUM_2;  // R4
    bus_cfg.pin_d13 = GPIO_NUM_42; // R5
    bus_cfg.pin_d14 = GPIO_NUM_41; // R6
    bus_cfg.pin_d15 = GPIO_NUM_40; // R7

    bus_cfg.freq_write = 16000000;

    bus_cfg.hsync_front_porch = 8;
    bus_cfg.hsync_pulse_width = 4;
    bus_cfg.hsync_back_porch  = 8;

    bus_cfg.vsync_front_porch = 8;
    bus_cfg.vsync_pulse_width = 4;
    bus_cfg.vsync_back_porch  = 8;

    bus_cfg.pclk_idle_high = true;

    _bus_instance.config(bus_cfg);
    _panel_instance.setBus(&_bus_instance);
}
