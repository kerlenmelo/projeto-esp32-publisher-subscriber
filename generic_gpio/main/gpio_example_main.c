#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

// --- Bibliotecas FreeRTOS ---
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h" // Necessário para sincronização Wi-Fi

// --- Bibliotecas ESP-IDF ---
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_log.h"        // Para mensagens de log
#include "nvs_flash.h"      // Para NVS (Non-Volatile Storage) - Requisito do Wi-Fi
#include "esp_wifi.h"       // Para gerenciamento de Wi-Fi
#include "esp_event.h"      // Para gerenciamento de eventos (Wi-Fi, MQTT)
#include "esp_system.h"     // Para esp_get_idf_version, esp_get_minimum_free_heap_size
#include "esp_crt_bundle.h" // Para certificados TLS (necessário para mqtts://)

// --- Definições de Configuração ---
#define GPIO_INPUT_IO_0 19                             // Pino GPIO conectado ao botão
#define GPIO_INPUT_PIN_SEL ((1ULL << GPIO_INPUT_IO_0)) // Máscara de bit para o pino

// --- Adições para Wi-Fi e MQTT ---
#define WIFI_SSID "DTEL_Edson"    // !!! SUBSTITUA PELO SEU SSID !!!
#define WIFI_PASSWORD "Siq.1905" // !!! SUBSTITUA PELA SUA SENHA !!!
// Use o hostname se possível, ou o IP se souber que é estático.
// Se o HiveMQ Cloud for este endereço, mantenha.
#define MQTT_BROKER_URI "mqtts://b0fcdfe8076b49ddac2de7428ed9b3cc.s1.eu.hivemq.cloud"
#define MQTT_BROKER_PORT 8883
#define MQTT_USERNAME "kerlen"           // Seu usuário HiveMQ
#define MQTT_PASSWORD "Kerlen123"        // Sua senha HiveMQ
#define MQTT_TOPIC_BUTTON "/esp32/botao" // Tópico MQTT para o botão
#define DEBOUNCE_TIME_MS 200             // Tempo de debounce em milissegundos

// --- Variáveis Globais ---
static const char *TAG = "MQTT_PUBLISHER";  // Tag para logs
static EventGroupHandle_t wifi_event_group; // Grupo de eventos para sincronização Wi-Fi
const int CONNECTED_BIT = BIT0;             // Bit que indica que o Wi-Fi está conectado

esp_mqtt_client_handle_t client = NULL; // Definindo o cliente MQTT globalmente e inicializando com NULL

// --- Funções de Callback de Eventos Wi-Fi ---
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

// --- Funções de Callback de Eventos MQTT ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    // O 'client' global já é acessível aqui, não precisa re-declarar 'client = event->client;'
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: Conectado ao broker MQTT");
        // No Publisher, geralmente não se subscreve a tópicos,
        // a menos que você queira receber confirmações ou comandos.
        // Se você quer que este ESP32 também seja um subscriber, deixe esta parte.
        // Para o objetivo inicial de "Publisher" (enviar o estado do botão), esta subscrição não é necessária aqui.
        // Se esta é a parte do Publisher, remova a subscrição ou mova-a para o Subscriber.
        // Para o Publisher, se você está se inscrevendo, significa que ele também é um Subscriber.
        // Por enquanto, vou manter o que você tinha, mas com a ressalva.
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
        // Este é um publisher, não deveria receber dados, a menos que esteja subscrito a tópicos.
        // No seu caso, ele está subscrito a /esp32/botao.
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

// --- Função de Inicialização do Wi-Fi ---
static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    // !!! ORDEM CORRETA DE INICIALIZAÇÃO DA PILHA DE REDE !!!
    ESP_ERROR_CHECK(esp_netif_init());                // Inicializa o netif (interface de rede)
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Cria o loop de eventos padrão

    esp_netif_create_default_wifi_sta(); // Cria a interface padrão Wi-Fi station

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // Inicializa o driver Wi-Fi

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL)); // Passa NULL para instance handle se não for desregistrar
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL)); // Passa NULL para instance handle

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // Ou o modo de autenticação da sua rede
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

// --- Função de Inicialização do MQTT ---
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtts://b0fcdfe8076b49ddac2de7428ed9b3cc.s1.eu.hivemq.cloud",
        .broker.address.port = 8883,
        .credentials.username = "kerlen",
        .credentials.authentication.password = "Kerlen123",
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .broker.verification.use_global_ca_store = false,               // Não usar a loja global de CA
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach, // Usar o bundle de certificados padrão
    };

    client = esp_mqtt_client_init(&mqtt_cfg); // Inicializa o cliente MQTT
    // Passa 'client' como o último argumento para o handler, se quiser acessá-lo por lá
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client); // Inicia a conexão com o broker MQTT
}

// --- Tarefa para monitorar o botão e publicar MQTT ---
void button_mqtt_publish_task(void *pvParameters) // Correção do tipo de parâmetro
{
    int button_state;
    int last_button_state = 1; // 1 = não pressionado (com pull-up)
    TickType_t last_press_time = xTaskGetTickCount();

    // Variável de estado para controlar se o LED deve estar "ligado" ou "desligado"
    bool is_led_on = false; // Começa como desligado

    while (1)
    {
        button_state = gpio_get_level(GPIO_INPUT_IO_0);

        // Detecta a borda de descida (botão pressionado: de HIGH para LOW)
        if (button_state == 0 && last_button_state == 1)
        {
            TickType_t current_time = xTaskGetTickCount();
            // Verifica o tempo de debounce para evitar múltiplos disparos
            // Nota: Adicionei o asterisco '*' que estava faltando em 'portTICK_PERIOD_MS'
            if ((current_time - last_press_time) * portTICK_PERIOD_MS > DEBOUNCE_TIME_MS)
            {
                if (client != NULL)
                {
                    const char *msg_to_publish;

                    if (is_led_on) {
                        // Se estava ligado, agora envia "OFF"
                        msg_to_publish = "OFF";
                        is_led_on = false; // Atualiza o estado
                    } else {
                        // Se estava desligado, agora envia "ON"
                        msg_to_publish = "ON";
                        is_led_on = true; // Atualiza o estado
                    }

                    ESP_LOGI(TAG, "Botão Pressionado! Publicando '%s' no tópico: %s", msg_to_publish, MQTT_TOPIC_BUTTON);
                    esp_mqtt_client_publish(client, MQTT_TOPIC_BUTTON, msg_to_publish, 0, 1, 0); // Publica a mensagem
                }
                else
                {
                    ESP_LOGW(TAG, "Cliente MQTT não inicializado, não é possível publicar.");
                }
                last_press_time = current_time; // Reseta o tempo da última pressão válido
            }
        }
        last_button_state = button_state; // Atualiza o último estado do botão

        vTaskDelay(pdMS_TO_TICKS(10)); // Pequeno delay para polling do botão (10ms é um bom valor)
    }
}

// --- Função Principal (app_main) ---
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Inicializando...");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    // 1. Inicializa o NVS (Non-Volatile Storage) - Primeiro passo para Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase()); // Apaga e tenta inicializar novamente se houver problema
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Inicializa Wi-Fi (inclui esp_netif_init() e esp_event_loop_create_default() internamente agora)
    wifi_init_sta(); // Esta função aguarda a conexão Wi-Fi

    // 3. Inicializa MQTT (só depois que o Wi-Fi estiver conectado)
    mqtt_app_start();

    // 4. Configuração do GPIO para o botão (pode ser feito aqui ou na tarefa)
    gpio_config_t io_conf_input = {};
    io_conf_input.intr_type = GPIO_INTR_DISABLE; // Não estamos usando interrupções aqui, apenas polling
    io_conf_input.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf_input.mode = GPIO_MODE_INPUT;
    io_conf_input.pull_up_en = 1;   // Habilita o pull-up interno
    io_conf_input.pull_down_en = 0; // Desabilita o pull-down
    gpio_config(&io_conf_input);
    ESP_LOGI(TAG, "GPIO %d configurado para input com pull-up.", GPIO_INPUT_IO_0);

    // 5. Cria a tarefa para monitorar o botão e publicar MQTT
    xTaskCreate(&button_mqtt_publish_task, "button_mqtt_publish_task", 4096, NULL, 5, NULL);
    // Nota: Removi o seu loop `while(1)` da `app_main`, pois ele não deve bloquear `app_main`.
    // As operações contínuas devem estar em tarefas FreeRTOS.
}