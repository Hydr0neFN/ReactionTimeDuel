/*
 * GT911 touch driver for the Waveshare ESP32-S3 Touch LCD 800x480 (DESIGN.md 7.2).
 * The touch reset line sits on the CH422G I/O expander, so the reset sequence is done here.
 */

#include "touch_gt911.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "touch";

#define TOUCH_SDA_GPIO      8
#define TOUCH_SCL_GPIO      9
#define TOUCH_INT_GPIO      4
#define TOUCH_I2C_HZ        400000
#define TOUCH_I2C_TIMEOUT   30
#define SCREEN_W            800
#define SCREEN_H            480
#define LONG_PRESS_MS       1000
/* A missing "new data" flag shortly after a clear is normal at the 100 Hz report rate: keep the
 * finger down for a few empty reads so a long press is not cut. */
#define RELEASE_AFTER_MISSES 3

#define CH422G_ADDR_MODE    0x24
#define CH422G_ADDR_OUT     0x38

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_gt;
static uint16_t s_res_x = SCREEN_W;
static uint16_t s_res_y = SCREEN_H;

static esp_err_t expander_write(uint8_t addr, uint8_t value)
{
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = TOUCH_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit(dev, &value, 1, TOUCH_I2C_TIMEOUT);
    i2c_master_bus_rm_device(dev);
    return err;
}

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_gt, r, sizeof(r), buf, len, TOUCH_I2C_TIMEOUT);
}

static esp_err_t gt_write_u8(uint16_t reg, uint8_t value)
{
    const uint8_t w[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value };
    return i2c_master_transmit(s_gt, w, sizeof(w), TOUCH_I2C_TIMEOUT);
}

bool touch_gt911_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Reset sequence of the Waveshare demo. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    err = expander_write(CH422G_ADDR_MODE, 0x01);               /* expander outputs enabled */
    if (err == ESP_OK) {
        err = expander_write(CH422G_ADDR_OUT, 0x2C);            /* touch reset low, backlight on */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G not answering: %s", esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TOUCH_INT_GPIO, 0);                           /* INT low while reset is released: 0x5D */
    vTaskDelay(pdMS_TO_TICKS(100));
    expander_write(CH422G_ADDR_OUT, 0x2E);                       /* touch reset high */
    vTaskDelay(pdMS_TO_TICKS(200));
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);

    static const uint8_t addrs[] = { 0x5D, 0x14 };
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (i2c_master_probe(s_bus, addrs[i], TOUCH_I2C_TIMEOUT) == ESP_OK) {
            addr = addrs[i];
            break;
        }
    }
    if (addr == 0) {
        ESP_LOGW(TAG, "GT911 not found on 0x5D / 0x14");
        return false;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = TOUCH_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_gt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GT911 add device failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t res[4];
    if (gt_read(0x8048, res, sizeof(res)) == ESP_OK) {
        uint16_t rx = (uint16_t)(res[0] | (res[1] << 8));
        uint16_t ry = (uint16_t)(res[2] | (res[3] << 8));
        if (rx != 0 && ry != 0) {
            s_res_x = rx;
            s_res_y = ry;
        }
    }
    ESP_LOGI(TAG, "GT911 at 0x%02X, resolution %ux%u", addr, s_res_x, s_res_y);
    return true;
}

bool touch_gt911_read(uint16_t *x, uint16_t *y)
{
    static bool down;
    static uint16_t last_x, last_y;
    static uint8_t misses;

    if (!s_gt) {
        return false;
    }
    uint8_t status = 0;
    if (gt_read(0x814E, &status, 1) != ESP_OK) {
        status = 0;
    }
    if (status & 0x80) {
        uint8_t n = status & 0x0F;
        uint8_t p[4];
        if (n >= 1 && n <= 5 && gt_read(0x8150, p, sizeof(p)) == ESP_OK) {
            uint32_t px = (uint32_t)(p[0] | (p[1] << 8));
            uint32_t py = (uint32_t)(p[2] | (p[3] << 8));
            if (s_res_x != SCREEN_W) px = px * SCREEN_W / s_res_x;
            if (s_res_y != SCREEN_H) py = py * SCREEN_H / s_res_y;
            last_x = (uint16_t)(px >= SCREEN_W ? SCREEN_W - 1 : px);
            last_y = (uint16_t)(py >= SCREEN_H ? SCREEN_H - 1 : py);
            down = true;
            misses = 0;
        } else if (n == 0) {
            if (++misses >= RELEASE_AFTER_MISSES) down = false;
        }
        gt_write_u8(0x814E, 0);
    } else if (down && ++misses >= RELEASE_AFTER_MISSES) {
        down = false;
    }
    if (down) {
        *x = last_x;
        *y = last_y;
    }
    return down;
}

static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    uint16_t x, y;
    if (touch_gt911_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void touch_gt911_register_lvgl(void)
{
    static lv_indev_drv_t drv;
    lv_indev_drv_init(&drv);
    drv.type = LV_INDEV_TYPE_POINTER;
    drv.read_cb = indev_read_cb;
    drv.long_press_time = LONG_PRESS_MS;
    lv_indev_drv_register(&drv);
}
