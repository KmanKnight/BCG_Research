#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include <rom/ets_sys.h>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"


// ---------------- TODO ----------------
//
// 1. Normalize data output, get rid of noise if possible
// 2. Try and figure out wireless data uploads
// 3. Add sending data through USB (UART?) at data collection time (Not through SD)
//
// --------------------------------------

// ---------------- PINS ----------------
#define CONVST_PIN 0
#define ADC_DATA_PIN1 1
#define ADC_DATA_CLK1 21
#define ADC_DATA_PIN2 2
#define ADC_DATA_CLK2 22

#define FREQ 256 // Sample Frequency

#define SD_MISO_PIN 18
#define SD_MOSI_PIN 20
#define SD_CLK_PIN 19
#define SD_CS_PIN 23

// ---------------- GPIO REGISTERS ----------------
#define GPIO_OUT_REG       *((volatile uint32_t *) 0x60091004)
#define GPIO_IN_REG        *((volatile uint32_t *) 0x6009103C)

// ---------------- DATA ----------------
// Holder for data
typedef struct {
    uint64_t timestamp;
    float FL, FR, BL, BR;
} DataPoint;

static DataPoint dataBuffer;

static QueueHandle_t dataQueue;

bool sdAvailable = false;

// ---------------- SHIFT IN (REPLACEMENT) ----------------
//Shift in data from pin, Serial Data Reader
uint8_t shiftInCustom(uint32_t dataPin, uint32_t clkPin) {
    uint8_t value = 0;

    for (int i = 0; i < 8; i++) {
        GPIO_OUT_REG |= BIT(clkPin);   // clock HIGH
        ets_delay_us(1);

        value <<= 1;
        if (GPIO_IN_REG & BIT(dataPin)) {
            value |= 1;
        }

        GPIO_OUT_REG &= ~BIT(clkPin);  // clock LOW
        ets_delay_us(1);
    }

    return value;
}

// ---------------- ADC READ ----------------
//Read the ADCs on the board and add the data to a buffer for uploading
void readADCs() {
    ets_delay_us(10);

    uint64_t data1 = 0;
	uint64_t data2 = 0;

    for (int i = 0; i < 4; i++) {
        data1 = (data1 << 8) | shiftInCustom(ADC_DATA_PIN1, ADC_DATA_CLK1);
		data2 = (data2 << 8) | shiftInCustom(ADC_DATA_PIN2, ADC_DATA_CLK2);

    }
	// Data is stored as a 64-bit value, separated into 4 16-bit fields for each ADC, we only care about the 1st and 3rd field, hence we mask with 0xFFFF
    dataBuffer.FL = (data1 & 0xFFFF);
	dataBuffer.FR = (data1>>32 & 0xFFFF);
	dataBuffer.BL = (data2 & 0xFFFF);
	dataBuffer.BR = (data2>>32 & 0xFFFF);

    char line[128];
    snprintf(line, sizeof(line), "%llu,%.6f,%.6f,%.6f,%.6f\n",
             dataBuffer.timestamp,
             dataBuffer.FL,
             dataBuffer.FR,
             dataBuffer.BL,
             dataBuffer.BR);

    xQueueSendFromISR(dataQueue, line, NULL);
}
// ---------------- SD FUNCS ----------------------
#define MOUNT_POINT "/sdcard"

sdmmc_card_t* card;

//Initialize the SD card
void init_sd_card() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = host.slot;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card
    );
	sdAvailable = ret == ESP_OK ? true : false;
}

uint64_t timeOffset = 0;

//Make File if not existing
void init_file() {
    FILE* f = fopen("/sdcard/data.csv", "r+");

    if (!f) {
        // File doesn't exist → create it
        f = fopen("/sdcard/data.csv", "w");
        fprintf(f, "ts(us),FL,FR,BL,BR\n");
        fclose(f);
        return;
    }

    // Read first line
    char line[128];
    fgets(line, sizeof(line), f);

    if (strstr(line, "FL") == NULL) {
        // Missing header → rewrite
        fclose(f);
        f = fopen("/sdcard/data.csv", "w");
        fprintf(f, "ts(us),FL,FR,BL,BR\n");
        fclose(f);
        return;
    }
	
	
	
    // Find last line (for timestamp recovery)
	fseek(f, 0, SEEK_END);
    long pos = ftell(f);

    while (pos > 0) {
        fseek(f, --pos, SEEK_SET);
        if (fgetc(f) == '\n') break;
    }

    fgets(line, sizeof(line), f);

    char* token = strtok(line, ",");
    if (token) {
        timeOffset = atoll(token);
    }

    fclose(f);
}



// ---------------- TIMER CALLBACK ----------------
//Interrupt to sample and read data
void IRAM_ATTR onTimer(void* arg) {
    GPIO_OUT_REG ^= BIT(CONVST_PIN);

    dataBuffer.timestamp = esp_timer_get_time();

    if (GPIO_OUT_REG & BIT(CONVST_PIN)) {
        readADCs();
    }
}

// ---------------- SD TASK (SIMPLIFIED) ----------------


//Whole SD write task, needs to be reworked to be SD write and WIFI Upload
void sdTask(void *arg) {
    char buffer[128];

    while (true) {

        if (uxQueueMessagesWaiting(dataQueue) >= 450) {

            FILE* f = fopen("/sdcard/data.csv", "a");
            if (!f) {
                printf("File open failed\n");
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            for (int i = 0; i < 450; i++) {
                if (xQueueReceive(dataQueue, buffer, 0)) {
                    fputs(buffer, f);
                }
            }

            fclose(f);

        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ---------------- MAIN ----------------
//Main setup, initilizes pins and all modules.
void app_main(void) {

    // GPIO setup
    gpio_set_direction((gpio_num_t)CONVST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)ADC_DATA_CLK1, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)ADC_DATA_PIN1, GPIO_MODE_INPUT);

	// Queue
	dataQueue = xQueueCreate(600, 128);  //QUEUE MUST BE MADE BEFORE TIMER, OTHERWISE ESP PANICS AND CRASHES
	
    // Timer (ESP-IDF way)
    const esp_timer_create_args_t timer_args = {
        .callback = &onTimer,
        .name = "adc_timer"
    };

    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);
    esp_timer_start_periodic(timer, 1000000 / (2 * FREQ));
	
	//SD Begin
	ESP_LOGI("SD", "SD INIT");
	init_sd_card();
	if (sdAvailable) init_file();
	else ESP_LOGI("SD", "SD FAIL");
	

    // Task
    if (sdAvailable) xTaskCreate(sdTask, "sdTask", 4096, NULL, 1, NULL);
}