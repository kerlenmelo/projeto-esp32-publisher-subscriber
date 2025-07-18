#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"

#define GPIO_INPUT_IO_0 19
#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_INPUT_IO_0))

#define WIFI_SSID "DTEL_Edson"
#define WIFI_PASSWORD "Siq.1905"

#define MQTT_BROKER_URI "mqtts://b0fcdfe8076b49ddac2de7428ed9b3cc.s1.eu.hivemq.cloud"
#define MQTT_BROKER_PORT 8883
#define MQTT_USERNAME "kerlen"
#define MQTT_PASSWORD "Kerlen123"
#define MQTT_TOPIC_BUTTON "/esp32/botao"
#define DEBOUNCE_TIME_MS 200

static const char *TAG = "MQTT_PUBLISHER";
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
esp_mqtt_client_handle_t client = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Desconectado do Wi-Fi. Tentando reconectar...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        ESP_LOGI(TAG, "Conectado ao Wi-Fi e IP obtido!");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: Conectado ao broker MQTT");
        msg_id = esp_mqtt_client_subscribe(client, MQTT_TOPIC_BUTTON, 1);
        ESP_LOGI(TAG, "Inscrito no tópico: %s (msg_id=%d)", MQTT_TOPIC_BUTTON, msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED: MQTT desconectado.");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED: Mensagem enviada, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA: TOPIC=%.*s, DATA=%.*s", event->topic_len, event->topic, event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR: Erro no MQTT");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "Last error code reported from tls stack: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "Last error code reported from system errno: %d", event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        ESP_LOGI(TAG, "Outro evento MQTT id:%d", event->event_id);
        break;
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Esperando por conexão Wi-Fi...");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi conectado com sucesso!");
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtts://b0fcdfe8076b49ddac2de7428ed9b3cc.s1.eu.hivemq.cloud",
        .broker.address.port = 8883,
        .credentials.username = "kerlen",
        .credentials.authentication.password = "Kerlen123",
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .broker.verification.use_global_ca_store = false,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void button_mqtt_publish_task(void *pvParameters)
{
    int button_state;
    int last_button_state = 1;
    TickType_t last_press_time = xTaskGetTickCount();
    bool is_led_on = false;

    while (1)
    {
        button_state = gpio_get_level(GPIO_INPUT_IO_0);

        if (button_state == 0 && last_button_state == 1)
        {
            TickType_t current_time = xTaskGetTickCount();

            if ((current_time - last_press_time) * portTICK_PERIOD_MS > DEBOUNCE_TIME_MS)
            {
                if (client != NULL)
                {
                    const char *msg_to_publish;

                    if (is_led_on)
                    {
                        msg_to_publish = "OFF";
                        is_led_on = false;
                    }
                    else
                    {
                        msg_to_publish = "ON";
                        is_led_on = true;
                    }

                    ESP_LOGI(TAG, "Botão Pressionado! Publicando '%s' no tópico: %s", msg_to_publish, MQTT_TOPIC_BUTTON);
                    esp_mqtt_client_publish(client, MQTT_TOPIC_BUTTON, msg_to_publish, 0, 1, 0);
                }
                else
                {
                    ESP_LOGW(TAG, "Cliente MQTT não inicializado, não é possível publicar.");
                }

                last_press_time = current_time;
            }
        }

        last_button_state = button_state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Inicializando...");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    mqtt_app_start();

    gpio_config_t io_conf_input = {};
    io_conf_input.intr_type = GPIO_INTR_DISABLE;
    io_conf_input.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf_input.mode = GPIO_MODE_INPUT;
    io_conf_input.pull_up_en = 1;
    io_conf_input.pull_down_en = 0;
    gpio_config(&io_conf_input);
    ESP_LOGI(TAG, "GPIO %d configurado para input com pull-up.", GPIO_INPUT_IO_0);

    xTaskCreate(&button_mqtt_publish_task, "button_mqtt_publish_task", 4096, NULL, 5, NULL);
}
