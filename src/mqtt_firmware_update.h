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


void request_firmware_info(void);
void handle_firmware_info(const uint8_t *data, size_t len);
void process_firmware_chunk(const uint8_t *data, size_t len, int chunk_num);
void get_firmware_chunk(int chunk_num);

#endif // MQTT_FIRMWARE_UPDATE_H
