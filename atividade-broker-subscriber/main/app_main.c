/*
 * Projeto: Controle de LED via MQTT no ESP32
 * Descricao: ESP32 atua como cliente MQTT que se conecta a um broker, se inscreve
 *            em um tópico e aciona um LED com base nas mensagens recebidas ("1" liga, "0" desliga).
 * Broker usado: HiveMQ Cloud (com conexão segura TLS)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"

#define LED_GPIO_PIN GPIO_NUM_2 // GPIO do LED embutido
static const char *TAG = "mqtt5_led_control";

/**
 * @brief Handler de eventos MQTT
 *
 * Este handler é chamado para tratar todos os eventos do cliente MQTT (conexão, recebimento de dados, etc.)
 */
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Evento MQTT recebido: base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    static int msg_id;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado ao broker MQTT");

        // Inscreve no tópico que controla o LED
        msg_id = esp_mqtt_client_subscribe(client, "/esp32/botao", 1);
        ESP_LOGI(TAG, "Inscrito no tópico: /esp32/botao (msg_id=%d)", msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Desconectado do broker MQTT");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Inscrição realizada (msg_id=%d)", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Mensagem recebida do broker");
        ESP_LOGI(TAG, "TOPICO=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DADO=%.*s", event->data_len, event->data);

        // Lógica de controle do LED
        if (strncmp((char *)event->data, "ON", event->data_len) == 0)
        {
            gpio_set_level(LED_GPIO_PIN, 1);
            ESP_LOGI(TAG, "LED ligado");
        }
        else if (strncmp((char *)event->data, "OFF", event->data_len) == 0)
        {
            gpio_set_level(LED_GPIO_PIN, 0);
            ESP_LOGI(TAG, "LED desligado");
        }
        else
        {
            ESP_LOGW(TAG, "Comando desconhecido: %.*s", event->data_len, event->data);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Erro no cliente MQTT");
        break;

    default:
        ESP_LOGI(TAG, "Outro evento MQTT recebido (id=%d)", event->event_id);
        break;
    }
}

/**
 * @brief Inicializa e inicia o cliente MQTT v5
 */
static void mqtt5_app_start(void)
{
    esp_mqtt_client_config_t mqtt5_cfg = {
        .broker.address.uri = "mqtts://b0fcdfe8076b49ddac2de7428ed9b3cc.s1.eu.hivemq.cloud",
        .broker.address.port = 8883,
        .credentials.username = "kerlen",
        .credentials.authentication.password = "Kerlen123",
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .broker.verification.use_global_ca_store = false,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);

    // Registra o handler para qualquer evento MQTT
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);

    // Inicia o cliente MQTT
    esp_mqtt_client_start(client);
}

/**
 * @brief Função principal da aplicação
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando aplicação...");
    ESP_LOGI(TAG, "Memória livre: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Versão do IDF: %s", esp_get_idf_version());

    // Inicializa o GPIO do LED como saída
    gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_OUTPUT);

    // Inicializa os componentes de rede e armazenamento
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Conecta-se à rede Wi-Fi conforme definido nas configurações do menuconfig
    ESP_ERROR_CHECK(example_connect());

    // Inicia o cliente MQTT
    mqtt5_app_start();
}
