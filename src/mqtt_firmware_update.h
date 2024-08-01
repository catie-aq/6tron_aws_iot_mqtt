#ifndef MQTT_FIRMWARE_UPDATE_H
#define MQTT_FIRMWARE_UPDATE_H

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>

struct firmware_info {
    char title[64];
    char version[16];
    int size;
};

extern struct mqtt_client client_ctx;
extern int firmware_request_id;
extern int chunk_count;
extern int firmware_chunk_size;

static char current_firmware_version[24]; 
static char current_firmware_title[64];   

int request_firmware_info();
void handle_firmware_info(const uint8_t *data, size_t len);
void process_firmware_chunk(const uint8_t *data, size_t len, int chunk_num);
int get_firmware(int chunk_number); // Add this line
int update_request_topic_name(char *topic_name, int chunk_number);
int send_message(char *topic, char *payload);
char *current_firmware_to_json();
int store_firmware_chunk(void *payload, int chunk_number, int chunk_len);

int on_connect();

static ssize_t process_message(const struct mqtt_publish_param *pub);

#endif // MQTT_FIRMWARE_UPDATE_H