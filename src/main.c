#include "creds/creds.h"
#include "dhcp.h"
#include "mqtt_firmware_update.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/time.h>
#include <zephyr/random/random.h> 
#if defined(CONFIG_MBEDTLS_MEMORY_DEBUG)
#include <mbedtls/memory_buffer_alloc.h>
#endif

LOG_MODULE_REGISTER(tb, LOG_LEVEL_DBG);

#define SNTP_SERVER "0.pool.ntp.org"
#define TB_BROKER_PORT "8883"

#define MQTT_BUFFER_SIZE 256u
#define APP_BUFFER_SIZE 4096u

#define MAX_RETRIES 10u
#define BACKOFF_CONST_MS 5000u
#define SLEEP_TIME_MS 1000

static struct sockaddr_in tb_broker;

static uint8_t rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_BUFFER_SIZE];
static uint8_t buffer[APP_BUFFER_SIZE];

static const char mqtt_client_name[] = CONFIG_TB_THING_NAME;

static uint32_t messages_received_counter;
static bool do_publish;
static bool do_subscribe;

#define TLS_TAG_DEVICE_CERTIFICATE 1
#define TLS_TAG_DEVICE_PRIVATE_KEY 1
#define TLS_TAG_TB_CA_CERTIFICATE 2

static const sec_tag_t sec_tls_tags[] = {
    TLS_TAG_DEVICE_CERTIFICATE,
    TLS_TAG_TB_CA_CERTIFICATE,
};

static int subscribe_to_topics(void) {
    int ret;
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
         .qos = MQTT_QOS_1_AT_LEAST_ONCE}
    };
    const struct mqtt_subscription_list sub_list = {
        .list = topics,
        .list_count = ARRAY_SIZE(topics),
        .message_id = 1u,
    };

    LOG_INF("Subscribing to %hu topic(s)", sub_list.list_count);

    ret = mqtt_subscribe(&client_ctx, &sub_list);
    if (ret != 0) {
        LOG_ERR("Failed to subscribe to topics: %d", ret);
    }

    return ret;
}

static int publish_message(const char *topic, size_t topic_len,
                           uint8_t *payload, size_t payload_len) {
    static uint32_t message_id = 1u;

    int ret;
    struct mqtt_publish_param msg;

    msg.retain_flag = 0u;
    msg.message.topic.topic.utf8 = topic;
    msg.message.topic.topic.size = topic_len;
    msg.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    msg.message.payload.data = payload;
    msg.message.payload.len = payload_len;
    msg.message_id = message_id++;

    ret = mqtt_publish(&client_ctx, &msg);
    if (ret != 0) {
        LOG_ERR("Failed to publish message: %d", ret);
    }

    LOG_INF("PUBLISHED on topic \"%s\" [ id: %u qos: %u ], payload: %zu B", topic,
            msg.message_id, msg.message.topic.qos, payload_len);
    LOG_HEXDUMP_DBG(payload, payload_len, "Published payload:");

    return ret;
}

static ssize_t handle_published_message(const struct mqtt_publish_param *pub) {
    int ret;
    size_t received = 0u;
    const size_t message_size = pub->message.payload.len;
    const bool discarded = message_size > APP_BUFFER_SIZE;

    LOG_INF("RECEIVED on topic \"%s\" [ id: %u qos: %u ] payload: %u / %u B",
            (const char *)pub->message.topic.topic.utf8, pub->message_id,
            pub->message.topic.qos, message_size, APP_BUFFER_SIZE);

     while (received < message_size) {
        uint8_t *p = discarded ? buffer : &buffer[received];

        ret = mqtt_read_publish_payload_blocking(&client_ctx, p, APP_BUFFER_SIZE);
        if (ret < 0) {
            return ret;
        }

        received += ret;    
    }
   
    LOG_HEXDUMP_DBG(buffer, MIN(message_size, 256u), "Received payload:");

    process_message(pub, buffer, message_size);
    
    return 0;
}

const char *mqtt_evt_type_to_str(enum mqtt_evt_type type) {
    static const char *const types[] = {
        "CONNACK", "DISCONNECT", "PUBLISH", "PUBACK",   "PUBREC",
        "PUBREL",  "PUBCOMP",    "SUBACK",  "UNSUBACK", "PINGRESP",
    };

    return (type < ARRAY_SIZE(types)) ? types[type] : "<unknown>";
}

static void mqtt_event_cb(struct mqtt_client *client,
                          const struct mqtt_evt *evt) {
    LOG_DBG("MQTT event: %s [%u] result: %d", mqtt_evt_type_to_str(evt->type),
            evt->type, evt->result);

    switch (evt->type) {
        case MQTT_EVT_CONNACK: {
            do_subscribe = true;
        } break;

        case MQTT_EVT_PUBLISH: {
            const struct mqtt_publish_param *pub = &evt->param.publish;

            handle_published_message(pub);
            messages_received_counter++;
            do_publish = true;
        } break;

        case MQTT_EVT_SUBACK: {
            do_publish = true;
        } break;

        case MQTT_EVT_PUBACK:
        case MQTT_EVT_DISCONNECT:
        case MQTT_EVT_PUBREC:
        case MQTT_EVT_PUBREL:
        case MQTT_EVT_PUBCOMP:
        case MQTT_EVT_PINGRESP:
        case MQTT_EVT_UNSUBACK:
        default:
            break;
    }
}

static void tb_client_setup(void) {
    mqtt_client_init(&client_ctx);

    client_ctx.broker = &tb_broker;
    client_ctx.evt_cb = mqtt_event_cb;
    client_ctx.client_id.utf8 = (uint8_t *)mqtt_client_name;
    client_ctx.client_id.size = sizeof(mqtt_client_name) - 1;
    client_ctx.password = NULL;
    client_ctx.user_name = NULL;
    client_ctx.keepalive = CONFIG_MQTT_KEEPALIVE;
    client_ctx.protocol_version = MQTT_VERSION_3_1_1;

    client_ctx.rx_buf = rx_buffer;
    client_ctx.rx_buf_size = MQTT_BUFFER_SIZE;
    client_ctx.tx_buf = tx_buffer;
    client_ctx.tx_buf_size = MQTT_BUFFER_SIZE;

    client_ctx.transport.type = MQTT_TRANSPORT_SECURE;
    struct mqtt_sec_config *const tls_config = &client_ctx.transport.tls.config;

    tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
    tls_config->cipher_list = NULL;
    tls_config->sec_tag_list = sec_tls_tags;
    tls_config->sec_tag_count = ARRAY_SIZE(sec_tls_tags);
    tls_config->hostname = CONFIG_TB_ENDPOINT;
    tls_config->cert_nocopy = TLS_CERT_NOCOPY_NONE;
}

struct backoff_context {
    uint16_t retries_count;
    uint16_t max_retries;
};

static void backoff_context_init(struct backoff_context *bo) {
    __ASSERT_NO_MSG(bo != NULL);

    bo->retries_count = 0u;
    bo->max_retries = MAX_RETRIES;
}

static void backoff_get_next(struct backoff_context *bo,
                             uint32_t *next_backoff_ms) {
    __ASSERT_NO_MSG(bo != NULL);
    __ASSERT_NO_MSG(next_backoff_ms != NULL);

    *next_backoff_ms = BACKOFF_CONST_MS;
}

static int tb_client_try_connect(void) {
    int ret;
    uint32_t backoff_ms;
    struct backoff_context bo;

    backoff_context_init(&bo);

    while (bo.retries_count <= bo.max_retries) {
        ret = mqtt_connect(&client_ctx);
        if (ret == 0) {
            goto exit;
        }

        backoff_get_next(&bo, &backoff_ms);

        LOG_ERR("Failed to connect: %d backoff delay: %u ms", ret, backoff_ms);
        k_msleep(backoff_ms);
    }

exit:
    return ret;
}

struct publish_payload {
    uint32_t counter;
};

static const struct json_obj_descr json_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct publish_payload, counter, JSON_TOK_NUMBER),
};

static int publish(void) {
    struct publish_payload pl = {.counter = messages_received_counter};

    json_obj_encode_buf(json_descr, ARRAY_SIZE(json_descr), &pl, buffer,
                        sizeof(buffer));

    return publish_message(CONFIG_TB_PUBLISH_TOPIC,
                           strlen(CONFIG_TB_PUBLISH_TOPIC), buffer,
                           strlen(buffer));
}

void tb_client_loop(void) {
    int rc;
    int timeout;
    struct zsock_pollfd fds;

    tb_client_setup();

    rc = tb_client_try_connect();
    if (rc != 0) {
        goto cleanup;
    }

    fds.fd = client_ctx.transport.tcp.sock;
    fds.events = ZSOCK_POLLIN;

    for (;;) {
        timeout = mqtt_keepalive_time_left(&client_ctx);
        rc = zsock_poll(&fds, 1u, timeout);
        if (rc >= 0) {
            if (fds.revents & ZSOCK_POLLIN) {
                rc = mqtt_input(&client_ctx);
                if (rc != 0) {
                    LOG_ERR("Failed to read MQTT input: %d", rc);
                    break;
                }
            }

            if (fds.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
                LOG_ERR("Socket closed/error");
                break;
            }

            rc = mqtt_live(&client_ctx);
            if ((rc != 0) && (rc != -EAGAIN)) {
                LOG_ERR("Failed to live MQTT: %d", rc);
                break;
            }
        } else {
            LOG_ERR("poll failed: %d", rc);
            break;
        }

        if (do_publish) {
            do_publish = false;
            publish();
        }

        if (do_subscribe) {
            do_subscribe = false;
            subscribe_to_topics();
            request_firmware_info();
        }

        // if (do_firmware_update) {
        //     do_firmware_update = false;
        //     get_firmware(chunk_number);
        // }
    }

cleanup:
    mqtt_disconnect(&client_ctx);

    zsock_close(fds.fd);
    fds.fd = -1;
}

int sntp_sync_time(void) {
    int rc;
    struct sntp_time now;
    struct timespec tspec;

    rc = sntp_simple(SNTP_SERVER, SYS_FOREVER_MS, &now);
    if (rc == 0) {
        tspec.tv_sec = now.seconds;
        tspec.tv_nsec = ((uint64_t)now.fraction * (1000lu * 1000lu * 1000lu)) >> 32;

        clock_settime(CLOCK_REALTIME, &tspec);

        LOG_DBG("Acquired time from NTP server: %u", (uint32_t)tspec.tv_sec);
    } else {
        LOG_ERR("Failed to acquire SNTP, code %d\n", rc);
    }
    return rc;
}

static int resolve_broker_addr(struct sockaddr_in *broker) {
    int ret;
    struct zsock_addrinfo *ai = NULL;

    const struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = 0,
    };

    ret = zsock_getaddrinfo(CONFIG_TB_ENDPOINT, TB_BROKER_PORT, &hints, &ai);
    if (ret == 0) {
        char addr_str[INET_ADDRSTRLEN];

        memcpy(broker, ai->ai_addr,
               MIN(ai->ai_addrlen, sizeof(struct sockaddr_storage)));

        zsock_inet_ntop(AF_INET, &broker->sin_addr, addr_str, sizeof(addr_str));
        LOG_INF("Resolved: %s:%u", addr_str, htons(broker->sin_port));
    } else {
        LOG_ERR("failed to resolve hostname err = %d (errno = %d)", ret, errno);
    }

    zsock_freeaddrinfo(ai);

    return ret;
}

static int setup_credentials(void) {
    int ret;

    ret = tls_credential_add(TLS_TAG_DEVICE_CERTIFICATE,
                             TLS_CREDENTIAL_SERVER_CERTIFICATE, public_cert,
                             public_cert_len);
    if (ret < 0) {
        LOG_ERR("Failed to add device certificate: %d", ret);
        goto exit;
    }

    ret = tls_credential_add(TLS_TAG_DEVICE_PRIVATE_KEY, TLS_CREDENTIAL_PRIVATE_KEY,
                             private_key, private_key_len);
    if (ret < 0) {
        LOG_ERR("Failed to add device private key: %d", ret);
        goto exit;
    }

    ret = tls_credential_add(TLS_TAG_TB_CA_CERTIFICATE,
                             TLS_CREDENTIAL_CA_CERTIFICATE, ca_cert, ca_cert_len);
    if (ret < 0) {
        LOG_ERR("Failed to add device CA certificate: %d", ret);
        goto exit;
    }

exit:
    return ret;
}

int main(void) {
#if defined(CONFIG_NET_DHCPV4)
    app_dhcpv4_startup();
#endif

    sntp_sync_time();

    setup_credentials();

    for (;;) {
        resolve_broker_addr(&tb_broker);

        tb_client_loop();

#if defined(CONFIG_MBEDTLS_MEMORY_DEBUG)
        size_t cur_used, cur_blocks, max_used, max_blocks;

        mbedtls_memory_buffer_alloc_cur_get(&cur_used, &cur_blocks);
        mbedtls_memory_buffer_alloc_max_get(&max_used, &max_blocks);
        LOG_INF("mbedTLS heap usage: MAX %zu/%u (%u) CUR %zu (%u)", max_used,
                CONFIG_MBEDTLS_HEAP_SIZE, max_blocks, cur_used, cur_blocks);
#endif

        k_sleep(K_SECONDS(1));
    }

    return 0;
}
