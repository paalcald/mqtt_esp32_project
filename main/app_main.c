/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_err.h"
#include "esp_check.h"
#include "esp_event_base.h"
#include "esp_system.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "shtc3.h"
#include "cbor.h"

#define CBOR_MAP_ERR(a, goto_tag, log_message) \
    if ((a) != CborNoError)       \
    {                             \
        ret = a;                  \
        goto goto_tag;            \
    }

#define INITIAL_MEASUREMENT_INTERVAL 1
#define MAX_TOPIC_LEN 16

#define DEFAULT_QOS 0
#define MAX_CBOR_BUFFER_SIZE 1024

#define BUILDING_NAME "EDIFICIO_PAALCALD"
#define BUILDING_STORY "P_1"
#define BUILDING_WING "N"
#define BUILDING_ROOM "1"
#define TEMP_EXTENSION "TEMP"
#define HUM_EXTENSION "HUM"
#define LUX_EXTENSION "LUX"
#define VIBR_EXTENSION "VIBR"
#define TEMP_MSG_KEY "temperatura"
#define HUM_MSG_KEY "humedad"
#define LUX_MSG_KEY "luz"
#define VIBR_MSG_KEY "vibracion"
#define ENABLE_EXTENSION "enable"
#define DISABLE_EXTENSION "disable"
#define INTERVAL_EXTENSION "interval"
#define ENABLED_SENSOR_FLAG (1 << 0)
#define TEMP_SENSOR_FLAG (1 << 1)
#define HUM_SENSOR_FLAG (1 << 2)
#define LUX_SENSOR_FLAG (1 << 3)
#define VIBR_SENSOR_FLAG (1 << 4)

static const char *TAG = "mqtt_example";
float temp;
float hum;
shtc3_t tempSensor;
uint8_t cbor_buffer[MAX_CBOR_BUFFER_SIZE];
size_t cbor_buffer_size;
esp_mqtt_client_handle_t mqtt_client;
i2c_master_bus_handle_t bus_handle;
uint8_t enabled_sensors;
esp_timer_handle_t measurement_timer_handle = NULL;
uint64_t measurement_interval = INITIAL_MEASUREMENT_INTERVAL;
esp_mqtt_topic_t subscribed_topics[3];
esp_mqtt_topic_t publishing_topic;

static void periodic_temp_measurement_callback(void *args)
{
    if (shtc3_get_temp_and_hum(&tempSensor, &temp, &hum)) {
        ESP_LOGE(TAG, "Could not get temperature");
    } else {
        CborEncoder measurement_encoder;
        cbor_encoder_init(&measurement_encoder, cbor_buffer, MAX_CBOR_BUFFER_SIZE, 0);
        CborEncoder temp_encoder;
        cbor_encoder_create_map(&measurement_encoder, &temp_encoder, 1);
        cbor_encode_text_stringz(&temp_encoder, TEMP_MSG_KEY);
        cbor_encode_float(&temp_encoder, temp);
        cbor_encoder_close_container(&measurement_encoder, &temp_encoder);
        cbor_buffer_size = cbor_encoder_get_buffer_size(&measurement_encoder, cbor_buffer);
        ESP_LOGI(TAG,"Temperature encoded");
        //it will send temperature to every topic untill i figure out how to avoid repeated code.
        if (enabled_sensors & TEMP_SENSOR_FLAG) {
            esp_mqtt_client_publish(mqtt_client, "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/" TEMP_EXTENSION, (char*) cbor_buffer, cbor_buffer_size, publishing_topic.qos, 0);
        }
        if (enabled_sensors & HUM_SENSOR_FLAG) {
            esp_mqtt_client_publish(mqtt_client, "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/" HUM_EXTENSION, (char*) cbor_buffer, cbor_buffer_size, publishing_topic.qos, 0);
        }
        if (enabled_sensors & LUX_SENSOR_FLAG) {
            esp_mqtt_client_publish(mqtt_client, "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/" LUX_EXTENSION, (char*) cbor_buffer, cbor_buffer_size, publishing_topic.qos, 0);
        }
        if (enabled_sensors & VIBR_SENSOR_FLAG) {
            esp_mqtt_client_publish(mqtt_client, "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/" VIBR_EXTENSION, (char*) cbor_buffer, cbor_buffer_size, publishing_topic.qos, 0);
        }
    }
}

void init_i2c(void)
{
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = 8,
        .sda_io_num = 10,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle));

    shtc3_init(&tempSensor, bus_handle, 0x70);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    // int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe_multiple(client, subscribed_topics, 3);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            esp_mqtt_client_reconnect(client);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            ESP_LOGI(TAG, "TOPIC=%.*s\n", event->topic_len, event->topic);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            // Check to what topic it belongs to
            ESP_LOGI(TAG, "TOPIC=%.*s\n", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Topics subscribed:");
            for (int i = 0; i < 3; i++){
                ESP_LOGI(TAG, "%s", subscribed_topics[i].filter);
            }
            char* building_name = strtok(event->topic, "/");
            char* building_story = strtok(NULL, "/");
            char* building_wing = strtok(NULL, "/");
            char* building_room = strtok(NULL, "/");
            char* sensor_type_or_interval = strtok(NULL, "/");
            size_t sensor_type_or_interval_len = event->topic_len - (sensor_type_or_interval - event->topic);
            if(strncmp(sensor_type_or_interval, INTERVAL_EXTENSION, sensor_type_or_interval_len) == 0) {
                event->data[event->data_len] = '\0';
                int maybe_interval = (int) strtol(event->data, (char**) NULL, 10);
                if (maybe_interval != 0) {
                    measurement_interval = maybe_interval;
                    ESP_LOGI(TAG, "Received interval=%llu", measurement_interval);
                    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_restart(measurement_timer_handle, measurement_interval * 1000));
                }
            } else {
                char* topic = strtok(NULL, "/");
                size_t topic_len = event->topic_len - (topic - event->topic);
                // optimization can be reached using lexicographic order and strcomp nature
                if (strncmp(sensor_type_or_interval, TEMP_EXTENSION, sensor_type_or_interval_len) == 0) {
                    if (strncmp(topic, ENABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & TEMP_SENSOR_FLAG) {
                            ESP_LOGI(TAG, "Attempted to enable to enable an already running sensor");
                        } else {
                            enabled_sensors |= TEMP_SENSOR_FLAG;
                        }
                    } else if (strncmp(topic, DISABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & TEMP_SENSOR_FLAG) {
                            enabled_sensors &= TEMP_SENSOR_FLAG;
                        } else {
                            ESP_LOGI(TAG, "Attempted to stop an already stopped sensor");
                        }
                    }
                } else if (strncmp(sensor_type_or_interval, HUM_EXTENSION, sensor_type_or_interval_len) == 0) {
                    if (strncmp(topic, ENABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & HUM_SENSOR_FLAG) {
                            ESP_LOGI(TAG, "Attempted to enable to enable an already running sensor");
                        } else {
                            enabled_sensors |= HUM_SENSOR_FLAG;
                        }
                    } else if (strncmp(topic, DISABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & HUM_SENSOR_FLAG) {
                            enabled_sensors &= HUM_SENSOR_FLAG;
                        } else {
                            ESP_LOGI(TAG, "Attempted to stop an already stopped sensor");
                        }
                    }
                } else if (strncmp(sensor_type_or_interval, LUX_EXTENSION, sensor_type_or_interval_len) == 0) {
                    if (strncmp(topic, ENABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & LUX_SENSOR_FLAG) {
                            ESP_LOGI(TAG, "Attempted to enable to enable an already running sensor");
                        } else {
                            enabled_sensors |= LUX_SENSOR_FLAG;
                        }
                    } else if (strncmp(topic, DISABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & LUX_SENSOR_FLAG) {
                            enabled_sensors &= LUX_SENSOR_FLAG;
                        } else {
                            ESP_LOGI(TAG, "Attempted to stop an already stopped sensor");
                        }
                    }
                } else if (strncmp(sensor_type_or_interval, VIBR_EXTENSION, sensor_type_or_interval_len) == 0) {
                    if (strncmp(topic, ENABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & VIBR_SENSOR_FLAG) {
                            ESP_LOGI(TAG, "Attempted to enable to enable an already running sensor");
                        } else {
                            enabled_sensors |= VIBR_SENSOR_FLAG;
                        }
                    } else if (strncmp(topic, DISABLE_EXTENSION, topic_len) == 0) {
                        if (enabled_sensors & VIBR_SENSOR_FLAG) {
                            enabled_sensors &= VIBR_SENSOR_FLAG;
                        } else {
                            ESP_LOGI(TAG, "Attempted to stop an already stopped sensor");
                        }
                    }
                }
                ESP_LOGI(TAG, "%.*s",sensor_type_or_interval_len, topic);
                if (!enabled_sensors & esp_timer_is_active(measurement_timer_handle)) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(measurement_timer_handle));
                } else if (enabled_sensors & !esp_timer_is_active(measurement_timer_handle)) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_periodic(measurement_timer_handle, measurement_interval * 1000));
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
        }
}

static void mqtt_app_start()
{
    // By default it connects with a clean session.
    // So disconnection means that server subscriptions need to be redone.
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .broker.address.port = 1883,
        .credentials.username = "friend",
        .credentials.authentication.password = "mellon",
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */
    // Initiate Topics
    subscribed_topics[0].filter = "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/" INTERVAL_EXTENSION ;
    subscribed_topics[0].qos = 0;
    subscribed_topics[1].filter = "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/+/" ENABLE_EXTENSION ;
    subscribed_topics[1].qos = 0;
    subscribed_topics[2].filter = "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/+/" DISABLE_EXTENSION ;
    subscribed_topics[2].qos = 0;

    // Publishing Topic
    publishing_topic.qos = 0;
    publishing_topic.filter = "/" BUILDING_NAME "/" BUILDING_STORY "/" BUILDING_WING "/" BUILDING_ROOM "/";

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, (void*) subscribed_topics);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    init_i2c();

    const esp_timer_create_args_t periodic_temp_measurement_args = {
        .callback = &periodic_temp_measurement_callback,
        .name = "measurement",
    };
    ESP_ERROR_CHECK(esp_timer_create(&periodic_temp_measurement_args, &measurement_timer_handle));

    mqtt_app_start();
}
