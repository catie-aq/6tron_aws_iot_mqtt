#include "mqtt_firmware_update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>

struct mqtt_client client_ctx;
int firmware_request_id = 10;
int chunk_count = 0;
int chunk_number = 0;
int firmware_chunk_size = 256;

bool do_firmware_update = false;

LOG_MODULE_DECLARE(tb, LOG_LEVEL_INF);

struct firmware_info {
    int fw_size;
    char fw_title[128];
    char fw_version[16];
};

static const struct json_obj_descr firmware_info_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct firmware_info, fw_size, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct firmware_info, fw_title, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct firmware_info, fw_version, JSON_TOK_STRING),
};

int extract_string(const char *json, const char *key, char *value) {
    struct firmware_info info;
    int ret = json_obj_parse((char *)json, strlen(json), firmware_info_descr, ARRAY_SIZE(firmware_info_descr), &info);
    if (ret < 0) {
        LOG_ERR("Failed to parse JSON: %d", ret);
        return ret;
    }

    if (strcmp(key, "fw_title") == 0) {
        strcpy(value, info.fw_title);
    } else if (strcmp(key, "fw_version") == 0) {
        strcpy(value, info.fw_version);
    } else {
        LOG_ERR("Key not found");
        return -1;
    }

    return 0;
}

int extract_number(const char *json, const char *key, int *value) {
    struct firmware_info info;
    int ret = json_obj_parse((char *)json, strlen(json), firmware_info_descr, ARRAY_SIZE(firmware_info_descr), &info);
    if (ret < 0) {
        LOG_ERR("Failed to parse JSON: %d", ret);
        return ret;
    }

    if (strcmp(key, "fw_size") == 0) {
        *value = info.fw_size;
    } else {
        LOG_ERR("Key not found");
        return -1;
    }

    return 0;
}

char *current_firmware_to_json() {
    static char firmware_infos[96];
    sprintf(firmware_infos,
            "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\"}",
            current_firmware_title, current_firmware_version);
    return firmware_infos;
}

int update_response_topic_name(char *topic_name) {
    sprintf(topic_name, "v2/fw/response/%d/chunk/", firmware_request_id);
    return 0;
}

int update_request_topic_name(char *topic_name, int chunk_number) {
    sprintf(topic_name, "v2/fw/request/%d/chunk/%d", firmware_request_id,
            chunk_number);
    return 0;
}

int send_message(char *topic, char *payload) {
    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.topic.topic.utf8 = topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.payload.data = payload;
    param.message.payload.len = strlen(payload);
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;

    int ret = mqtt_publish(&client_ctx, &param);
    if (ret) {
        LOG_ERR("Failed to publish message to topic %s: %d", topic, ret);
    } else {
        LOG_INF("Message published successfully to topic %s", topic);
    }
    return ret;
}

// Payload is the firmware binary chunk
int store_firmware_chunk(void *payload, int chunk_number, int chunk_len) {
    LOG_INF("TODO: write chunk %d (len:%d) to flash", chunk_number, chunk_len);

    // Store the firmware chunk in the flash memory
    // The firmware binary chunk is stored in the payload variable
    // flash_img_buffered_write()
    return 0;
}

int request_firmware_info() {
    LOG_INF("Requesting firmware info");
    char topic[100];
    sprintf(topic, "v1/devices/me/attributes/request/%d", firmware_request_id);
    char keys[] = "{\"sharedKeys\" : "
                  "\"fw_checksum,fw_checksum_algorithm,fw_size,fw_title,fw_"
                  "version,fw_state\"}";
    int rc = send_message(topic, keys);
    if (rc < 0) {
        LOG_ERR("Failed to request firmware info: %d", rc);
    } else {
        LOG_INF("Firmware info request published successfully");
    }
    return rc;
}

int send_telemetry(char *payload) {
    return send_message((char *)CONFIG_TB_PUBLISH_TOPIC, payload);
}

void process_firmware_chunk(const uint8_t *chunk, size_t chunk_size,
                            int chunk_num) {
    LOG_INF("Processing firmware chunk %d with size %zu", chunk_num, chunk_size);
}

int get_firmware(int chunk_number) {
    static char update_request_topic[256];
    int ret;

    ret = update_request_topic_name(update_request_topic, chunk_number);
    if (ret != 0) {
        LOG_ERR("Failed to update request topic name: %d", ret);
        return ret;
    }
    static char payload[100];
    sprintf(payload, "%d", firmware_chunk_size);
    send_message(update_request_topic, payload);

    return 0;
}

int on_connect() {
    int rc;

    struct mqtt_topic topics[] = {
        {.topic = {.utf8 = CONFIG_TB_SUBSCRIBE_TOPIC,
                   .size = strlen(CONFIG_TB_SUBSCRIBE_TOPIC)},
         .qos = MQTT_QOS_1_AT_LEAST_ONCE},
        {.topic = {.utf8 = "v1/devices/me/attributes/response/+",
                   .size = strlen("v1/devices/me/attributes/response/+")},
         .qos = MQTT_QOS_1_AT_LEAST_ONCE},
        {.topic = {.utf8 = "v2/fw/response/+",
                   .size = strlen("v2/fw/response/+")},
         .qos = MQTT_QOS_1_AT_LEAST_ONCE},
        {.topic = {.utf8 = "v2/fw/response/+/chunk/+",
                   .size = strlen("v2/fw/response/+/chunk/+")},
         .qos = MQTT_QOS_1_AT_LEAST_ONCE}};

    const struct mqtt_subscription_list sub_list = {
        .list = topics,
        .list_count = ARRAY_SIZE(topics),
        .message_id = 1u,
    };

    rc = mqtt_subscribe(&client_ctx, &sub_list);
    if (rc != 0) {
        LOG_ERR("Subscribe to topics failed: %d", rc);
        return rc;
    }

    LOG_INF("Subscribed to all topics successfully");

    rc = send_telemetry(current_firmware_to_json());
    if (rc != 0) {
        LOG_ERR("Failed to send telemetry data: %d", rc);
        return rc;
    }

    rc = request_firmware_info();
    if (rc != 0) {
        LOG_ERR("Failed to request firmware info: %d", rc);
        return rc;
    }

    rc = get_firmware(0);
    if (rc != 0) {
        LOG_ERR("Failed to start firmware download: %d", rc);
        return rc;
    }

    return 0;
}

ssize_t process_message(const struct mqtt_publish_param *pub, uint8_t *buff, size_t buff_len) {
    char update_response_topic[256];
    update_response_topic_name(update_response_topic);

    LOG_INF("Message arrived on topic %.*s", pub->message.topic.topic.size, pub->message.topic.topic.utf8);

    if (0 == strncmp(pub->message.topic.topic.utf8, "v1/devices/me/attributes", 24)) {
        if (strstr(pub->message.topic.topic.utf8, "/response/") != NULL) {
            /* Place null terminator at end of payload buffer */
            buff[buff_len] = '\0';

            LOG_INF("Payload: %.*s", (int)buff_len, buff);
            LOG_INF("Payload length: %zu", buff_len);

            char title[128];
            char version[16];
            int size;

            if (extract_string((const char *)buff, "fw_title", title) < 0) {
                LOG_ERR("Failed to extract firmware title");
                return -1;
            }

            if (extract_string((const char *)buff, "fw_version", version) < 0) {
                LOG_ERR("Failed to extract firmware version");
                return -1;
            }

            if (extract_number((const char *)buff, "fw_size", &size) < 0) {
                LOG_ERR("Failed to extract firmware size");
                return -1;
            }

            LOG_INF("Firmware title: %s", title);
            LOG_INF("Firmware version: %s", version);
            LOG_INF("Firmware size: %d", size);

            if (strcmp(version, current_firmware_version) != 0) {
                LOG_INF("New firmware version available: %s - %s", title, version);
                char telemetry_payload[200];
                snprintf(telemetry_payload, sizeof(telemetry_payload),
                         "{\"fw_state\" :\"DOWNLOADING\", \"current_fw_version\":\"%s\", "
                         "\"current_fw_title\":\"%s\"}",
                         current_firmware_version, current_firmware_title);
                send_telemetry(telemetry_payload);
                k_sleep(K_SECONDS(1));

                firmware_request_id++;

                chunk_count = (size + firmware_chunk_size - 1) / firmware_chunk_size;
                LOG_INF("Chunk count: %d", chunk_count);
                LOG_INF("chunk_count: %d", chunk_count);

                get_firmware(0);
            } else {
                LOG_INF("Firmware version is up to date: %s", version);
            }
        }
    } else if (0 == strncmp(pub->message.topic.topic.utf8, update_response_topic, strlen(update_response_topic))) {
        LOG_INF("\n\nFirmware Chunk received!\n\n");

        sscanf(pub->message.topic.topic.utf8, "v2/fw/response/%d/chunk/%d", &firmware_request_id, &chunk_number);

        store_firmware_chunk(buff, chunk_number, buff_len);

        if (chunk_number >= 10) {
            LOG_INF("Firmware download completed");
        } else {
            chunk_number++;
            get_firmware(chunk_number);
        }
    }

    return 0; // Adjust return value as needed
}
