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
int chunk_count = 0;            // number of chunks to download
int firmware_chunk_size = 4096; // define firmware_chunk_size

struct json_key_value {
  char value[128];
};

LOG_MODULE_DECLARE(tb);

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
  printf("Chunk %d\n", chunk_number);

  // Store the firmware chunk in the flash memory
  // The firmware binary chunk is stored in the payload variable

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
  return send_message((char *)"v1/devices/me/telemetry", payload);
}

int extract_string(const char *json, const char *key, char *value) {
  struct json_key_value kv;
  struct json_obj_descr descr[] = {
      JSON_OBJ_DESCR_PRIM_NAMED(struct json_key_value, value, value,
                                JSON_TOK_STRING),
  };

  int ret = json_obj_parse(json, strlen(json), descr, ARRAY_SIZE(descr), &kv);
  if (ret < 0) {
    LOG_ERR("Failed to extract string for key %s: %d", key, ret);
    return ret;
  }

  strcpy(value, kv.value);
  return 0;
}

int extract_number(const char *json, const char *key, int *value) {
  struct json_key_value kv;
  struct json_obj_descr descr[] = {
      JSON_OBJ_DESCR_PRIM_NAMED(struct json_key_value, value, value,
                                JSON_TOK_NUMBER),
  };

  int ret = json_obj_parse(json, strlen(json), descr, ARRAY_SIZE(descr), &kv);
  if (ret < 0) {
    LOG_ERR("Failed to extract number for key %s: %d", key, ret);
    return ret;
  }

  *value = kv.value;
  return 0;
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

    return 0; // Return 0 to indicate success
}

int on_connect() {
    int rc;

    struct mqtt_topic topics[] = {
        {.topic = {.utf8 = "v1/devices/me/attributes/response/+", .size = strlen("v1/devices/me/attributes/response/+")}, .qos = MQTT_QOS_1_AT_LEAST_ONCE},
        {.topic = {.utf8 = "v1/devices/me/attributes", .size = strlen("v1/devices/me/attributes")}, .qos = MQTT_QOS_1_AT_LEAST_ONCE},
        {.topic = {.utf8 = "v2/fw/response/+", .size = strlen("v2/fw/response/+")}, .qos = MQTT_QOS_1_AT_LEAST_ONCE},
        {.topic = {.utf8 = "v2/fw/response/+/chunk/+", .size = strlen("v2/fw/response/+/chunk/+")}, .qos = MQTT_QOS_1_AT_LEAST_ONCE}
    };

    const struct mqtt_subscription_list sub_list = {
        .list = topics,
        .list_count = ARRAY_SIZE(topics),
        .message_id = 1u,
    };

    rc = mqtt_subscribe(&client_ctx, &sub_list);
    if (rc != 0) {
        LOG_ERR("Subscribe to topics failed: %d", rc);
        return rc; // Return the error code
    }

    LOG_INF("Subscribed to all topics successfully");

    rc = send_telemetry(current_firmware_to_json());
    if (rc != 0) {
        LOG_ERR("Failed to send telemetry data: %d", rc);
        return rc; // Return the error code
    }

    rc = request_firmware_info();
    if (rc != 0) {
        LOG_ERR("Failed to request firmware info: %d", rc);
        return rc; // Return the error code
    }

    rc = get_firmware(0); // Start downloading firmware
    if (rc != 0) {
        LOG_ERR("Failed to start firmware download: %d", rc);
        return rc; // Return the error code
    }

    return 0; // Success
}


void handle_firmware_info(const uint8_t *payload, size_t payload_len) {
  char title[128];
  char version[128];
  int size;

  if (extract_string((const char *)payload, "fw_title", title) < 0) {
    LOG_ERR("Failed to extract firmware title");
    return;
  }

  if (extract_string((const char *)payload, "fw_version", version) < 0) {
    LOG_ERR("Failed to extract firmware version");
    return;
  }

  if (extract_number((const char *)payload, "fw_size", &size) < 0) {
    LOG_ERR("Failed to extract firmware size");
    return;
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

    get_firmware(0);
  } else {
    LOG_INF("Firmware version is up to date: %s", version);
  }
}