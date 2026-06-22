#include "epd.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_log.h>

/* Wired to the HAT's labeled 9-pin breakout (this unit's 40-pin header is
 * electrically dead — never use it). PWR gates the panel's high-voltage rail
 * through a MOSFET on the HAT: LOW = panel off, HIGH = on. */
#define PIN_MOSI  GPIO_NUM_18  /* XIAO D10 → HAT DIN  */
#define PIN_SCLK  GPIO_NUM_19  /* XIAO D8  → HAT CLK  */
#define PIN_CS    GPIO_NUM_17  /* XIAO D7  → HAT CS   */
#define PIN_DC    GPIO_NUM_21  /* XIAO D3  → HAT DC   */
#define PIN_RST   GPIO_NUM_2   /* XIAO D2  → HAT RST  */
#define PIN_BUSY  GPIO_NUM_1   /* XIAO D1  → HAT BUSY */
#define PIN_PWR   GPIO_NUM_16  /* XIAO D6  → HAT PWR  */

#define EPD_HOST      SPI2_HOST
#define EPD_CLOCK_HZ  (1 * 1000 * 1000)  /* match Waveshare's bit-banged speed */

static const char *TAG = "epd";
static spi_device_handle_t s_spi;

static void epd_gpio_init(void);
static void epd_spi_init(void);
static void epd_panel_init(void);
static void epd_refresh(void);
static void epd_wait_busy(const char *where);
static void epd_cmd(uint8_t c);
static void epd_data(uint8_t d);

void epd_init(void)
{
    epd_gpio_init();
    ESP_LOGI(TAG, "GPIO ready");
    epd_spi_init();
    ESP_LOGI(TAG, "SPI ready");
    epd_panel_init();
    ESP_LOGI(TAG, "panel init done");
}

void epd_display(const uint8_t *buf, size_t len)
{
    ESP_LOGI(TAG, "streaming %u bytes...", (unsigned)len);
    epd_cmd(0x10);
    for (size_t i = 0; i < len; i++) {
        epd_data(buf[i]);
    }
    ESP_LOGI(TAG, "pixels sent, refreshing (~15-25s)...");
    epd_refresh();
}

void epd_sleep(void)
{
    epd_cmd(0x07);
    epd_data(0xA5);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_PWR, 0);
}

static void epd_gpio_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_DC) | (1ULL << PIN_RST) | (1ULL << PIN_PWR),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(PIN_PWR, 1);   /* power the panel before any reset edge */
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void epd_spi_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,  /* manage CS manually */
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_HOST, &dev, &s_spi));
}

static void epd_reset(void)
{
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(200));
}

/* Register values transcribed from Waveshare's official EPD_3in6e driver —
 * byte-for-byte, do not tune. */
static void epd_panel_init(void)
{
    epd_reset();
    epd_wait_busy("post-reset");
    vTaskDelay(pdMS_TO_TICKS(30));

    epd_cmd(0xAA); epd_data(0x49); epd_data(0x55); epd_data(0x20);
                   epd_data(0x08); epd_data(0x09); epd_data(0x18);
    epd_cmd(0x01); epd_data(0x3F);
    epd_cmd(0x00); epd_data(0x5F); epd_data(0x69);
    epd_cmd(0x05); epd_data(0x40); epd_data(0x1F); epd_data(0x1F); epd_data(0x2C);
    epd_cmd(0x08); epd_data(0x6F); epd_data(0x1F); epd_data(0x1F); epd_data(0x22);
    epd_cmd(0x06); epd_data(0x6F); epd_data(0x1F); epd_data(0x17); epd_data(0x17);
    epd_cmd(0x03); epd_data(0x00); epd_data(0x54); epd_data(0x00); epd_data(0x44);
    epd_cmd(0x60); epd_data(0x02); epd_data(0x00);
    epd_cmd(0x30); epd_data(0x08);
    epd_cmd(0x50); epd_data(0x3F);
    epd_cmd(0x61); epd_data(0x01); epd_data(0x90); epd_data(0x02); epd_data(0x58);
    epd_cmd(0xE3); epd_data(0x2F);
    epd_cmd(0x84); epd_data(0x01);
    epd_wait_busy("post-init");
}

static void epd_refresh(void)
{
    epd_cmd(0x04);
    epd_wait_busy("power-on");
    vTaskDelay(pdMS_TO_TICKS(200));

    epd_cmd(0x06); epd_data(0x6F); epd_data(0x1F);
                   epd_data(0x16); epd_data(0x29);
    vTaskDelay(pdMS_TO_TICKS(200));

    epd_cmd(0x12); epd_data(0x00);
    epd_wait_busy("refresh");

    epd_cmd(0x02); epd_data(0x00);
    epd_wait_busy("power-off");
}

/* BUSY is LOW while the panel works. Healthy: post-reset ~130ms, power-on ~3s,
 * refresh 15-25s. The 120s ceiling keeps a power fault from bricking the boot. */
static void epd_wait_busy(const char *where)
{
    ESP_LOGI(TAG, "wait BUSY=1 (%s)", where);
    int ticks = 0;
    while (gpio_get_level(PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++ticks % 500 == 0) {
            ESP_LOGI(TAG, "  ...still waiting at %s (%ds)", where, ticks / 100);
        }
        if (ticks > 12000) {
            ESP_LOGE(TAG, "BUSY timeout at %s after 120s", where);
            return;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void epd_write_byte(uint8_t b, int dc)
{
    gpio_set_level(PIN_DC, dc);
    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &b };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    gpio_set_level(PIN_CS, 1);
}

static void epd_cmd(uint8_t c)  { epd_write_byte(c, 0); }
static void epd_data(uint8_t d) { epd_write_byte(d, 1); }
