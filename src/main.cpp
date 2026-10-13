#include <Arduino.h>
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* SPI Pin Configuration (HSPI / SPI2) */
#define PIN_NUM_MISO GPIO_NUM_19
#define PIN_NUM_MOSI GPIO_NUM_23
#define PIN_NUM_CLK  GPIO_NUM_18
#define PIN_NUM_CS   GPIO_NUM_5

#define BUFFER_SIZE_BYTES 1024  // 1 KB payload per DMA burst

static spi_device_handle_t spi_handle;
static uint8_t *dma_tx_buffer = NULL;
static SemaphoreHandle_t timerSemaphore;
static TaskHandle_t dmaWorkerTaskHandle = NULL;

/* -------------------------------------------------------------------------- */
/* 1. Hardware Timer ISR Callback (Periodic Tick Dispatcher)                  */
/* -------------------------------------------------------------------------- */
static void IRAM_ATTR onHardwareTimerCallback(void *arg) {
    BaseType_t high_task_awoken = pdFALSE;
    // Signal the DMA worker task from ISR context
    xSemaphoreGiveFromISR(timerSemaphore, &high_task_awoken);
    if (high_task_awoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* -------------------------------------------------------------------------- */
/* 2. Low-Level SPI & DMA Initialization (Direct Driver Configuration)       */
/* -------------------------------------------------------------------------- */
void Init_SPI_DMA(void) {
    esp_err_t ret;

    // Bus configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUFFER_SIZE_BYTES,
        .flags = SPICOMMON_BUSFLAG_MASTER
    };

    // Device interface configuration
    spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,                         // SPI Mode 0 (CPOL=0, CPHA=0)
        .duty_cycle_pos = 128,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = 10 * 1000 * 1000, // 10 MHz SPI Clock
        .input_delay_ns = 0,
        .spics_io_num = PIN_NUM_CS,        // Hardware CS pin
        .flags = 0,
        .queue_size = 7                    // Transaction queue depth
    };

    // Initialize SPI bus with DMA Channel 1 enabled
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        Serial.printf("[SPI ERR] Bus init failed: 0x%X\n", ret);
        return;
    }

    // Attach device to the initialized SPI bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        Serial.printf("[SPI ERR] Device add failed: 0x%X\n", ret);
        return;
    }

    // Allocate DMA-capable memory buffer (must reside in internal 32-bit aligned SRAM)
    dma_tx_buffer = (uint8_t *)heap_caps_malloc(BUFFER_SIZE_BYTES, MALLOC_CAP_DMA);
    if (dma_tx_buffer == NULL) {
        Serial.println("[MEM ERR] Failed to allocate DMA-capable memory!");
        return;
    }

    // Populate synthetic payload pattern
    for (int i = 0; i < BUFFER_SIZE_BYTES; i++) {
        dma_tx_buffer[i] = (uint8_t)(i & 0xFF);
    }

    Serial.println("[SPI-DMA] Controller initialized at 10 MHz with active DMA Channel.");
}

/* -------------------------------------------------------------------------- */
/* 3. Non-Blocking DMA Worker Task                                           */
/* -------------------------------------------------------------------------- */
void TaskDMAStreamer(void *pvParameters) {
    spi_transaction_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.length = BUFFER_SIZE_BYTES * 8; // Transaction length in bits
    trans.tx_buffer = dma_tx_buffer;
    trans.rx_buffer = NULL;               // Transmit only

    uint32_t burst_count = 0;

    for (;;) {
        // Wait for hardware timer interrupt notification
        if (xSemaphoreTake(timerSemaphore, portMAX_DELAY) == pdTRUE) {
            burst_count++;
            uint32_t cpu_start_cycles = xthal_get_ccount();

            // Enqueue non-blocking DMA transaction
            esp_err_t ret = spi_device_queue_trans(spi_handle, &trans, portMAX_DELAY);

            // CPU cycles spent simply enqueuing the hardware transfer
            uint32_t cpu_dispatch_cycles = xthal_get_ccount() - cpu_start_cycles;

            if (ret == ESP_OK) {
                // While the DMA hardware pumps bytes to the SPI bus independently,
                // the CPU is 100% free to execute other critical math or logic here.
                
                // Wait for hardware completion signal
                spi_transaction_t *completed_trans;
                ret = spi_device_get_trans_result(spi_handle, &completed_trans, portMAX_DELAY);

                if (ret == ESP_OK && (burst_count % 10 == 0)) {
                    Serial.printf("[DMA BURST #%04u] 1024 Bytes dispatched over SPI | CPU enqueuing cost: %u cycles\n",
                                  burst_count, cpu_dispatch_cycles);
                }
            } else {
                Serial.println("[DMA ERR] Transaction queue failed!");
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Setup & Main Loop                                                          */
/* -------------------------------------------------------------------------- */
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n=======================================================");
    Serial.println("  Non-Blocking Hardware SPI-DMA & High-Precision ISR");
    Serial.println("=======================================================\n");

    timerSemaphore = xSemaphoreCreateBinary();

    // 1. Initialize SPI Controller and DMA Subsystem
    Init_SPI_DMA();

    // 2. Spawn Real-Time DMA Dispatcher Task on Core 0
    xTaskCreatePinnedToCore(
        TaskDMAStreamer,
        "DMAStreamer",
        4096,
        NULL,
        3, // High priority
        &dmaWorkerTaskHandle,
        0
    );

    // 3. Configure High-Precision Periodic Hardware Timer (100 Hz / 10 ms interval)
    const esp_timer_create_args_t timer_args = {
        .callback = &onHardwareTimerCallback,
        .name = "dma_periodic_timer"
    };

    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 10000)); // 10,000 microseconds = 10 ms

    Serial.println("[TIMER] Hardware periodic interrupt active (100 Hz dispatch rate).");
}

void loop() {
    // Delete setup task; execution is entirely event-driven via ISR & FreeRTOS tasks
    vTaskDelete(NULL);
}