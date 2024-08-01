
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
int firmware_chunk_size = 3200;
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
  return send_message((char *)CONFIG_TB_PUBLISH_TOPIC, payload);
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
  static char payload[100];
  sprintf(payload, "%d", firmware_chunk_size );
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

    return 0;
  }
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


ssize_t process_message(const struct mqtt_publish_param *pub) {
    char update_response_topic[256];
    update_response_topic_name(update_response_topic);

    printf("Message arrived on topic %.*s\n",
           pub->message.topic.topic.size,
           pub->message.topic.topic.utf8);

    // Test if topic starts with v1/devices/me/attributes
    if (0 == strncmp(pub->message.topic.topic.utf8, "v1/devices/me/attributes", 24)) {
        // check if topic contains "response"
        if (strstr(pub->message.topic.topic.utf8, "/response/") != NULL) {
            // put \0 at the end of the message payload
            ((char *)pub->message.payload.data)[pub->message.payload.len] = '\0';
            printf("Payload: %s\n", (char *)pub->message.payload.data);

            // get firmware version
            static char version[100];
            extract_string((char *)pub->message.payload.data, (char *)"shared.fw_version", version);

            // get firmware title
            static char title[100];
            extract_string((char *)pub->message.payload.data, (char *)"shared.fw_title", title);

            // if version or title is different from current version, update
            if (strcmp(version, current_firmware_version) != 0) {
                printf("New firmware version available: %s - %s\n", title, version);
                char telemetry_payload[200];
                sprintf(telemetry_payload,
                        "{\"fw_state\" :\"DOWNLOADING\", \"current_fw_version\":\"%s\", "
                        "\"current_fw_title\":\"%s\"}",
                        current_firmware_version,
                        current_firmware_title);
                send_telemetry(telemetry_payload);
                k_sleep(K_SECONDS(1));

                firmware_request_id++;

                int firmware_size = 0;
                extract_number((char *)pub->message.payload.data, (char *)"shared.fw_size", &firmware_size);
                printf("Firmware size: %d\n", firmware_size);

                chunk_count = (firmware_size + firmware_chunk_size - 1) / firmware_chunk_size;
                printf("Chunk count: %d\n", chunk_count);

                // Assuming secondary_bd is initialized elsewhere
               /* if (secondary_bd->init() != 0) {
                    printf("Init failed\n");
                }*/

                get_firmware(0);
            } else {
                printf("Firmware version is up to date: %s\n", version);
            }
        }
    } else if (0 == strncmp(pub->message.topic.topic.utf8, update_response_topic, strlen(update_response_topic))) {
        int chunk_number = 0;
        sscanf(pub->message.topic.topic.utf8, "v2/fw/response/%d/chunk/%d", &firmware_request_id, &chunk_number);
        store_firmware_chunk(pub->message.payload.data, chunk_number, pub->message.payload.len);
        if (chunk_number < chunk_count - 1) {
            get_firmware(chunk_number + 1);
        } else {
            printf("Firmware download completed\n");
        }
    }

    return 0; // Adjust return value as needed
}
