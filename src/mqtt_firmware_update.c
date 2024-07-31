#include "mqtt_firmware_update.h"
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>


struct mqtt_client client_ctx;

int firmware_request_id = 10;
int chunk_count = 0; // number of chunks to download
int firmware_chunk_size = 4096; // define firmware_chunk_size

LOG_MODULE_DECLARE(tb);

void handle_firmware_info(const uint8_t *payload, size_t payload_len) {
    struct firmware_info firmware_info;
    struct json_obj_descr json_descr[] = {
        JSON_OBJ_DESCR_PRIM_NAMED(struct firmware_info, "fw_title", title, JSON_TOK_STRING),
        JSON_OBJ_DESCR_PRIM_NAMED(struct firmware_info, "fw_version", version, JSON_TOK_STRING),
        JSON_OBJ_DESCR_PRIM(struct firmware_info, size, JSON_TOK_NUMBER),
    };

    int ret = json_obj_parse((char *)payload, payload_len, json_descr, ARRAY_SIZE(json_descr), &firmware_info);
    if (ret < 0) {
        LOG_ERR("Failed to parse firmware info: %d", ret);
        return;
    }

    LOG_INF("Firmware title: %s", firmware_info.title);
    LOG_INF("Firmware version: %s", firmware_info.version);
    LOG_INF("Firmware size: %d", firmware_info.size);

    chunk_count = (firmware_info.size + firmware_chunk_size - 1) / firmware_chunk_size;
    get_firmware_chunk(0);
}

void process_firmware_chunk(const uint8_t *chunk, size_t chunk_size, int chunk_num) {
    LOG_INF("Processing firmware chunk %d with size %zu", chunk_num, chunk_size);
}

void request_firmware_info(void) {
    LOG_INF("Requesting firmware info");

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.topic.topic.utf8 = "v1/devices/me/attributes/request/1";
    param.message.topic.topic.size = strlen(param.message.topic.topic.utf8);
    param.message.payload.data = "{\"sharedKeys\": \"fw_title,fw_version,fw_checksum,fw_size\"}";
    param.message.payload.len = strlen(param.message.payload.data);
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;

    int ret = mqtt_publish(&client_ctx, &param);
    if (ret) {
        LOG_ERR("Failed to publish firmware info request: %d", ret);
    } else {
        LOG_INF("Firmware info request published successfully");
    }
}

void get_firmware_chunk(int chunk_num) {
    char topic[64];
    snprintf(topic, sizeof(topic), "v2/fw/request/%d/chunk/%d", firmware_request_id, chunk_num);

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.topic.topic.utf8 = topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.payload.data = NULL;
    param.message.payload.len = 0;
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;

    mqtt_publish(&client_ctx, &param);
}
