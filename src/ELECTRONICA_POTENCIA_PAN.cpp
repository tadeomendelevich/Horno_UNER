#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

// ================= WIFI STA =================
#define WIFI_SSID      "Tupescadorfavorito"
#define WIFI_PASS      "lucho12345"
#define MAX_RETRY      10

// ================= MQTT =================
#define MQTT_BROKER_URI  "mqtts://8bdae6c4d1a74a44b3d628e40808611f.s1.eu.hivemq.cloud:8883"
#define MQTT_USERNAME    "Tadeo"
#define MQTT_PASSWORD    "Uner2026"
#define TOPIC_DATA       "horno/datos"
#define TOPIC_CONTROL    "horno/control"

// ================= PINES =================
#define I2C_SDA             GPIO_NUM_21
#define I2C_SCL             GPIO_NUM_22
#define ZERO_CROSS_PIN      GPIO_NUM_27
#define TRIAC_PIN           GPIO_NUM_18

// ================= AM2320 =================
#define I2C_FREQ_HZ         100000
#define AM2320_ADDR         0x5C

// ================= CONTROL TRIAC =================
#define SEMICYCLE_US        10000
#define TRIAC_PULSE_US      200
#define SIMULAR_CRUCE       0
#define DEMO_LED_FISICO     0
#define DEMO_SEMICYCLE_MS   700
#define DEMO_PULSE_MS       120

static const char *TAG = "PAN_CONTROL";

// WiFi
static EventGroupHandle_t wifi_event_group;
static int retry_count = 0;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// I2C
static i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t am2320_handle;

// TRIAC
static QueueHandle_t zero_cross_queue;
static esp_timer_handle_t triac_fire_timer;
static esp_timer_handle_t triac_off_timer;

// MQTT
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Estado
static volatile int potencia_percent = 0;
static volatile int retardo_us = SEMICYCLE_US;
static volatile uint32_t zero_cross_count = 0;
static volatile uint32_t triac_fire_count = 0;
static float last_temp = 0.0f;
static float last_hum  = 0.0f;
static bool  sensor_ok = false;

// ================= RETARDO TRIAC =================
static int calcular_retardo_us(int p)
{
    if (p <= 0) return SEMICYCLE_US;
    if (p > 100) p = 100;
    int r = ((100 - p) * SEMICYCLE_US) / 100;
    if (r < 200)  r = 200;
    if (r > 9500) r = 9500;
    return r;
}

// ================= CRC AM2320 =================
static uint16_t crc16_modbus(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else               { crc >>= 1; }
        }
    }
    return crc;
}

// ================= LECTURA AM2320 =================
static esp_err_t am2320_read(float *temperature, float *humidity)
{
    uint8_t cmd[3] = {0x03, 0x00, 0x04};
    uint8_t data[8] = {0};

    (void)i2c_master_probe(i2c_bus_handle, AM2320_ADDR, 50);
    esp_rom_delay_us(1000);

    esp_err_t ret = i2c_master_transmit(am2320_handle, cmd, sizeof(cmd), 100);
    if (ret != ESP_OK) return ret;

    esp_rom_delay_us(2000);

    ret = i2c_master_receive(am2320_handle, data, sizeof(data), 100);
    if (ret != ESP_OK) return ret;

    if (data[0] != 0x03 || data[1] != 0x04) return ESP_ERR_INVALID_RESPONSE;

    uint16_t crc_rx   = data[6] | (data[7] << 8);
    uint16_t crc_calc = crc16_modbus(data, 6);
    if (crc_rx != crc_calc) return ESP_ERR_INVALID_CRC;

    uint16_t raw_hum  = (data[2] << 8) | data[3];
    uint16_t raw_temp = (data[4] << 8) | data[5];

    *humidity = raw_hum / 10.0f;
    if (raw_temp & 0x8000) {
        raw_temp &= 0x7FFF;
        *temperature = -(raw_temp / 10.0f);
    } else {
        *temperature = raw_temp / 10.0f;
    }
    return ESP_OK;
}

// ================= INIT I2C =================
static void i2c_init_am2320(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AM2320_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &am2320_handle));
    ESP_LOGI(TAG, "I2C listo: SDA GPIO21, SCL GPIO22");
}

// ================= TAREA SENSOR =================
static void sensor_task(void *pvParameters)
{
    while (1) {
        float t = 0.0f, h = 0.0f;
        esp_err_t ret = am2320_read(&t, &h);
        if (ret == ESP_OK) {
            last_temp = t;
            last_hum  = h;
            sensor_ok = true;
        } else {
            sensor_ok = false;
            ESP_LOGW(TAG, "No se pudo leer AM2320: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ================= TIMERS TRIAC =================
static void triac_off_callback(void *arg)
{
    gpio_set_level(TRIAC_PIN, 0);
}

static void triac_fire_callback(void *arg)
{
    gpio_set_level(TRIAC_PIN, 1);
    triac_fire_count++;
    esp_timer_stop(triac_off_timer);
#if DEMO_LED_FISICO == 1
    esp_timer_start_once(triac_off_timer, DEMO_PULSE_MS * 1000);
#else
    esp_timer_start_once(triac_off_timer, TRIAC_PULSE_US);
#endif
}

// ================= ISR CRUCE POR CERO =================
static void IRAM_ATTR zero_cross_isr_handler(void *arg)
{
    uint32_t event = 1;
    BaseType_t high_task_wakeup = pdFALSE;
    xQueueSendFromISR(zero_cross_queue, &event, &high_task_wakeup);
    if (high_task_wakeup) portYIELD_FROM_ISR();
}

// ================= TAREA TRIAC =================
static void triac_task(void *pvParameters)
{
    uint32_t event;
    int64_t last_zc_time = 0;

    while (1) {
        if (xQueueReceive(zero_cross_queue, &event, portMAX_DELAY)) {
            int64_t now = esp_timer_get_time();
#if DEMO_LED_FISICO == 0
            if ((now - last_zc_time) < 7000) continue;
#endif
            last_zc_time = now;
            zero_cross_count++;

            int p = potencia_percent;
            if (p <= 0) { gpio_set_level(TRIAC_PIN, 0); retardo_us = SEMICYCLE_US; continue; }
            if (p > 100) p = 100;
            retardo_us = calcular_retardo_us(p);

#if DEMO_LED_FISICO == 1
            int demo_r = ((100 - p) * DEMO_SEMICYCLE_MS * 1000) / 100;
            if (demo_r < 10000) demo_r = 10000;
            esp_timer_stop(triac_fire_timer);
            esp_timer_start_once(triac_fire_timer, demo_r);
#else
            esp_timer_stop(triac_fire_timer);
            esp_timer_start_once(triac_fire_timer, retardo_us);
#endif
        }
    }
}

// ================= SIMULADOR CRUCE =================
static void fake_zero_cross_task(void *pvParameters)
{
    uint32_t event = 1;
    while (1) {
        xQueueSend(zero_cross_queue, &event, 0);
#if DEMO_LED_FISICO == 1
        vTaskDelay(pdMS_TO_TICKS(DEMO_SEMICYCLE_MS));
#else
        vTaskDelay(pdMS_TO_TICKS(10));
#endif
    }
}

// ================= INIT TRIAC =================
static void triac_control_init(void)
{
    gpio_config_t triac_conf = {
        .pin_bit_mask  = (1ULL << TRIAC_PIN),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_ENABLE,
        .intr_type     = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&triac_conf));
    gpio_set_level(TRIAC_PIN, 0);

    gpio_config_t zc_conf = {
        .pin_bit_mask  = (1ULL << ZERO_CROSS_PIN),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_POSEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&zc_conf));

    zero_cross_queue = xQueueCreate(20, sizeof(uint32_t));

    esp_timer_create_args_t fire_args = { .callback = &triac_fire_callback, .name = "triac_fire" };
    esp_timer_create_args_t off_args  = { .callback = &triac_off_callback,  .name = "triac_off"  };
    ESP_ERROR_CHECK(esp_timer_create(&fire_args, &triac_fire_timer));
    ESP_ERROR_CHECK(esp_timer_create(&off_args,  &triac_off_timer));

#if SIMULAR_CRUCE == 0
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ZERO_CROSS_PIN, zero_cross_isr_handler, NULL));
    ESP_LOGI(TAG, "Cruce por cero real en GPIO27");
#else
    xTaskCreate(fake_zero_cross_task, "fake_zc", 2048, NULL, 10, NULL);
    ESP_LOGW(TAG, "MODO SIMULADO: cruce por cero falso");
#endif

    xTaskCreate(triac_task, "triac_task", 4096, NULL, 12, NULL);
    ESP_LOGI(TAG, "TRIAC_CTRL listo en GPIO18");
}

// ================= MQTT EVENTOS =================
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            ESP_LOGI(TAG, "MQTT conectado a HiveMQ Cloud");
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONTROL, 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT desconectado, reintentando...");
            break;

        case MQTT_EVENT_DATA: {
            char buf[64] = {0};
            int len = event->data_len < 63 ? event->data_len : 63;
            strncpy(buf, event->data, len);

            char *p = strstr(buf, "power");
            if (p) {
                p += 5;
                while (*p == ':' || *p == ' ' || *p == '"') p++;
                int pw = atoi(p);
                if (pw < 0)   pw = 0;
                if (pw > 100) pw = 100;
                potencia_percent = pw;
                retardo_us = calcular_retardo_us(pw);
                ESP_LOGI(TAG, "Potencia via MQTT: %d%%", pw);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Error MQTT");
            break;

        default:
            break;
    }
}

// ================= INIT MQTT =================
static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.credentials.username = MQTT_USERNAME;
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT iniciado: %s", MQTT_BROKER_URI);
}

// ================= TAREA MQTT PUBLISH =================
static void mqtt_publish_task(void *pvParameters)
{
    char payload[256];
    while (1) {
        if (mqtt_connected) {
            snprintf(payload, sizeof(payload),
                "{\"sensor_ok\":%s,\"temp\":%.1f,\"hum\":%.1f,\"power\":%d,"
                "\"delay_us\":%d,\"pulse_us\":%d,\"semi_us\":%d,"
                "\"zc\":%lu,\"fire\":%lu}",
                sensor_ok ? "true" : "false",
                last_temp, last_hum, potencia_percent,
                retardo_us, TRIAC_PULSE_US, SEMICYCLE_US,
                (unsigned long)zero_cross_count,
                (unsigned long)triac_fire_count);
            esp_mqtt_client_publish(mqtt_client, TOPIC_DATA, payload, 0, 1, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ================= WIFI EVENTOS STA =================
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Reintentando WiFi (%d/%d)...", retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi OK. IP local: " IPSTR " (acceso local disponible)", IP2STR(&ev->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ================= INIT WIFI STA =================
static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,   ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,     IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid,     WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Conectando a WiFi '%s'...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi conectado");
    } else {
        ESP_LOGE(TAG, "Fallo WiFi");
    }
}

// ================= WEB ROOT (acceso local) =================
static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!DOCTYPE html><html lang='es'><head>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Horno - Control Local</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0;}"
        "body{font-family:sans-serif;background:#0f172a;color:#f8fafc;padding:16px;}"
        ".wrap{max-width:480px;margin:0 auto;}"
        ".card{background:#1e293b;border-radius:16px;padding:22px;border:1px solid #334155;margin-bottom:14px;}"
        ".title{font-size:18px;font-weight:700;color:#60a5fa;text-align:center;margin-bottom:18px;}"
        ".sensors{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:16px;}"
        ".s{background:#0f172a;border-radius:12px;padding:16px;text-align:center;border:1px solid #334155;}"
        ".lbl{font-size:11px;color:#64748b;letter-spacing:1.2px;text-transform:uppercase;margin-bottom:8px;}"
        ".num{font-size:46px;font-weight:800;line-height:1;}"
        ".t{color:#f97316;}.h{color:#38bdf8;}.p{color:#a855f7;}"
        "hr{border:none;border-top:1px solid #334155;margin:14px 0;}"
        ".pow-row{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px;}"
        "input[type=range]{width:100%;height:7px;margin-bottom:14px;-webkit-appearance:none;appearance:none;background:#334155;border-radius:4px;outline:none;border:none;cursor:pointer;}"
        "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:#a855f7;border:3px solid #0f172a;cursor:pointer;}"
        ".btns{display:grid;grid-template-columns:repeat(5,1fr);gap:7px;margin-bottom:14px;}"
        ".btn{padding:10px 4px;border-radius:9px;border:1px solid #334155;background:#0f172a;color:#94a3b8;font-size:13px;font-weight:600;cursor:pointer;}"
        ".emg{width:100%;padding:15px;border-radius:12px;border:none;background:linear-gradient(135deg,#dc2626,#991b1b);color:white;font-size:17px;font-weight:800;cursor:pointer;letter-spacing:0.5px;}"
        ".badge{display:inline-flex;align-items:center;gap:7px;font-size:13px;font-weight:600;padding:6px 16px;border-radius:20px;margin-bottom:14px;}"
        ".b-ok{background:#052e16;color:#22c55e;border:1px solid #15803d;}"
        ".b-err{background:#2d0d0d;color:#f87171;border:1px solid #991b1b;}"
        ".dot{width:7px;height:7px;border-radius:50%;}"
        ".d-ok{background:#22c55e;box-shadow:0 0 7px #22c55e;}"
        ".d-err{background:#f87171;}"
        ".dbg{font-size:11px;color:#475569;font-family:monospace;background:#0f172a;padding:7px 10px;border-radius:8px;margin-top:12px;border:1px solid #1e293b;}"
        ".note{font-size:11px;color:#475569;text-align:center;margin-top:10px;}"
        "</style></head>"
        "<body><div class='wrap'>"
        "<div class='card'>"
        "<div class='title'>Horno de Leudado &mdash; Control Local</div>"
        "<div class='sensors'>"
        "<div class='s'><div class='lbl'>Temperatura</div><div class='num t'><span id='t'>--</span><span style='font-size:18px;color:#94a3b8;'> C</span></div></div>"
        "<div class='s'><div class='lbl'>Humedad</div><div class='num h'><span id='h'>--</span><span style='font-size:18px;color:#94a3b8;'> %</span></div></div>"
        "</div>"
        "<div style='text-align:center'><div id='st' class='badge b-ok'><div class='dot d-ok'></div>Leyendo...</div></div>"
        "<hr>"
        "<div class='pow-row'><span class='lbl'>Potencia TRIAC</span><span id='p' class='num p' style='font-size:40px;'>0%</span></div>"
        "<input id='sl' type='range' min='0' max='100' value='0' oninput='set(this.value)'>"
        "<div class='btns'>"
        "<button class='btn' style='border-color:#450a0a;color:#fca5a5;' onclick='set(0)'>OFF</button>"
        "<button class='btn' onclick='set(25)'>25%</button>"
        "<button class='btn' onclick='set(50)'>50%</button>"
        "<button class='btn' onclick='set(75)'>75%</button>"
        "<button class='btn' style='border-color:#064e3b;color:#6ee7b7;' onclick='set(100)'>MAX</button>"
        "</div>"
        "<button class='emg' onclick='set(0)'>&#9632; PARADA DE EMERGENCIA</button>"
        "<div id='dbg' class='dbg'>Esperando datos...</div>"
        "<p class='note'>Acceso remoto disponible desde el dashboard en Vercel</p>"
        "</div></div>"
        "<script>"
        "function set(v){"
        "v=parseInt(v);"
        "document.getElementById('sl').value=v;"
        "document.getElementById('p').innerHTML=v+'%';"
        "fetch('/set?power='+v);"
        "}"
        "function upd(){"
        "fetch('/data').then(function(r){return r.json();}).then(function(j){"
        "document.getElementById('t').innerHTML=j.temp.toFixed(1);"
        "document.getElementById('h').innerHTML=j.hum.toFixed(1);"
        "document.getElementById('p').innerHTML=j.power+'%';"
        "document.getElementById('sl').value=j.power;"
        "document.getElementById('dbg').innerHTML="
        "'Retardo: '+j.delay_us+' us | Pulso: '+j.pulse_us+' us | ZC: '+j.zc+' | Fires: '+j.fire;"
        "var st=document.getElementById('st');"
        "if(j.sensor_ok){"
        "st.innerHTML='<div class=\"dot d-ok\"></div>Sensor OK';"
        "st.className='badge b-ok';"
        "}else{"
        "st.innerHTML='<div class=\"dot d-err\"></div>Sin sensor';"
        "st.className='badge b-err';"
        "}"
        "}).catch(function(){"
        "var st=document.getElementById('st');"
        "st.innerHTML='<div class=\"dot d-err\"></div>Sin conexion';"
        "st.className='badge b-err';"
        "});}"
        "upd();setInterval(upd,2000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ================= WEB DATA =================
static esp_err_t data_get_handler(httpd_req_t *req)
{
    char response[256];
    httpd_resp_set_type(req, "application/json");
    snprintf(response, sizeof(response),
        "{\"sensor_ok\":%s,\"temp\":%.1f,\"hum\":%.1f,\"power\":%d,"
        "\"delay_us\":%d,\"pulse_us\":%d,\"semi_us\":%d,\"zc\":%lu,\"fire\":%lu}",
        sensor_ok ? "true" : "false",
        last_temp, last_hum, potencia_percent,
        retardo_us, TRIAC_PULSE_US, SEMICYCLE_US,
        (unsigned long)zero_cross_count,
        (unsigned long)triac_fire_count);
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

// ================= WEB SET POWER =================
static esp_err_t set_get_handler(httpd_req_t *req)
{
    char query[64], value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "power", value, sizeof(value)) == ESP_OK) {
            int p = atoi(value);
            if (p < 0)   p = 0;
            if (p > 100) p = 100;
            potencia_percent = p;
            retardo_us = calcular_retardo_us(p);
            ESP_LOGI(TAG, "Potencia (HTTP local): %d%%", p);
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// ================= WEB SERVER =================
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando servidor web");
        return NULL;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",     .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL },
        { .uri = "/data", .method = HTTP_GET, .handler = data_get_handler, .user_ctx = NULL },
        { .uri = "/set",  .method = HTTP_GET, .handler = set_get_handler,  .user_ctx = NULL },
    };
    for (int i = 0; i < 3; i++) httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "Servidor web local listo en puerto 80");
    return server;
}

// ================= APP MAIN =================
extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    i2c_init_am2320();
    triac_control_init();
    wifi_init_sta();
    mqtt_init();

    xTaskCreate(sensor_task,       "sensor_task",  4096, NULL, 5, NULL);
    xTaskCreate(mqtt_publish_task, "mqtt_publish", 4096, NULL, 5, NULL);

    start_webserver();

    ESP_LOGI(TAG, "Sistema listo.");
    ESP_LOGI(TAG, "  - Control remoto: dashboard Vercel via MQTT");
    ESP_LOGI(TAG, "  - Control local:  http://<IP_local>");
}
