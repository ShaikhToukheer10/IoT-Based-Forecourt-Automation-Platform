#include <cstdlib>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_pos_server.hpp"
#include "database.hpp"
#include "factory_reset.hpp"
#include "hub_identity.hpp"
#include "log_groups.hpp"
#include "reset_log.hpp"
#include "mesh_io.hpp"
#include "peripheral_config.hpp"
#include "queue_registry.hpp"
#include "rtc.hpp"
#include "sim_detect.hpp"
#include "web_server.hpp"
#include "sdkconfig.h"

/* Internal-RAM settings from sdkconfig.defaults. An sdkconfig left over from an older
 * checkout keeps its old values (defaults only fill missing keys) and the hub runs out of
 * internal RAM at boot: delete sdkconfig and build again. See docs/Memory_And_Tasks.md. */
#if !CONFIG_SPIRAM || !CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP || CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL > 512
#error "stale sdkconfig (PSRAM / malloc split): rm sdkconfig, then idf.py build"
#endif
#if !CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC || CONFIG_MBEDTLS_HARDWARE_AES || CONFIG_MBEDTLS_HARDWARE_SHA
#error "stale sdkconfig (mbedTLS: PSRAM buffers, software AES/SHA): rm sdkconfig, then idf.py build"
#endif
#if !CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH || !CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH || CONFIG_ESP_PHY_IRAM_OPT
#error "stale sdkconfig (FreeRTOS / heap / PHY code in flash): rm sdkconfig, then idf.py build"
#endif
#if CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM > 4 || (CONFIG_BT_ENABLED && !CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL)
#error "stale sdkconfig (Wi-Fi static RX buffers / NimBLE memory): rm sdkconfig, then idf.py build"
#endif
#if !CONFIG_HUB_SIM_BRINGUP || !CONFIG_UART_ISR_IN_IRAM
#error "stale sdkconfig (SIM bring-up before Wi-Fi / UART ISR in IRAM): rm sdkconfig, then idf.py build"
#endif

static const char* TAG = "main";

/* Internal-RAM readings at each boot stage (this is the scarce resource: BLE + USB modem + Wi-Fi). */
static void log_heap(const char* stage)
{
    ESP_LOGI(TAG, "heap[%s] internal free=%u largest=%u", stage,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

static esp_err_t init_nvs()
{
    auto err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

/* cJSON makes one small allocation per node; below SPIRAM_MALLOC_ALWAYSINTERNAL they would all
 * land in internal RAM (a site config parse alone is hundreds of nodes). */
static void* cjson_malloc(size_t n)
{
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

extern "C" void app_main(void)
{
    cJSON_Hooks hooks = {cjson_malloc, free};
    cJSON_InitHooks(&hooks);
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "HPCL IoT Hub boot — firmware %s (built %s %s)", app->version, app->date, app->time);

    ESP_ERROR_CHECK(init_nvs());
    hub::identity::factory_reset_if_requested();
    hub::identity::reset_log_record();
    hub::identity::log_groups_apply();
    hub::identity::start_uart_console();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Full NimBLE stack before WiFi — host sync + advertising fail if started after SoftAP. */
    ESP_ERROR_CHECK(hub::ble::init_queues());
    const bool uses_ble = hub::peripheral_config::saved_role_uses_ble();
    if (uses_ble) {
        ESP_ERROR_CHECK(hub::app::reserve_critical_task_stacks());
        const esp_err_t ble_err = hub::ble::start_gatt_service();
        if (ble_err != ESP_OK)
            ESP_LOGW(TAG, "Pine BLE not ready at boot (%s) — ble_pos will retry",
                     esp_err_to_name(ble_err));
    } else {
        /* ATG / RDU hub: no POS. Give the BLE controller + host RAM back to Wi-Fi and the tasks. */
        const esp_err_t rel = esp_bt_mem_release(ESP_BT_MODE_BLE);
        ESP_LOGI(TAG, "ATG/RDU role — Pine POS BLE off, BT RAM released (%s), internal free=%u",
                 esp_err_to_name(rel), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    }

    log_heap("after BLE");

    // Wall-clock for transaction stamps (PCB DS3231: SDA=IO11, SCL=IO10).
    const esp_err_t rtc_err = hub::rtc::init();
    if (rtc_err != ESP_OK)
        ESP_LOGW(TAG, "RTC init failed (%s) — txn times fall back until set",
                 esp_err_to_name(rtc_err));

#if CONFIG_HUB_SIM_BRINGUP
    /* USB CDC + AT before Wi-Fi: starting the USB host after SoftAP fails with ESP_ERR_NO_MEM. */
    const esp_err_t sim_err = hub::sim_detect::bringup_before_wifi();
    if (sim_err != ESP_OK)
        ESP_LOGW(TAG, "sim_detect: %s - mesh/SoftAP anyway; cellular auto-retries USB",
                 esp_err_to_name(sim_err));
#endif
    log_heap("after modem");

    ESP_ERROR_CHECK(hub::mesh::init());
    log_heap("after mesh init");
    hub::rtc::start_internet_time();
    ESP_ERROR_CHECK(hub::web::start_softap());
    /* Start httpd before DB/tasks so SoftAP UI has sockets + internal RAM. */
    const esp_err_t http_err = hub::web::start_http_ui();
    if (http_err != ESP_OK)
        ESP_LOGW(TAG, "HTTP UI start deferred (%s) — http_ui task will retry",
                 esp_err_to_name(http_err));
    log_heap("after http");
    ESP_ERROR_CHECK(hub::database::init());
    log_heap("after database");
    hub::ble::refresh_adv_ro_from_db();
    ESP_ERROR_CHECK(hub::peripheral_config::init());
    /* init() re-saves the role from peripheral.json; if it differs from the one BLE was sized
     * for (NVS lost, or a role change without reboot), reboot once so boot sizing matches. */
    if (hub::peripheral_config::saved_role_uses_ble() != uses_ble) {
        ESP_LOGW(TAG, "role changed since BLE setup (POS BLE %s → %s) — rebooting",
                 uses_ble ? "on" : "off", uses_ble ? "off" : "on");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    ESP_ERROR_CHECK(hub::app::queue_registry_init());
    log_heap("before tasks");
    ESP_ERROR_CHECK(hub::app::create_all_tasks());

    ESP_LOGI(TAG, "Base tasks created — internal free=%u largest=%u, PSRAM free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
