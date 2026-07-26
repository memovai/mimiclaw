#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize the MQTT bot.
 * Loads configuration from NVS or build-time secrets.
 */
esp_err_t mqtt_bot_init(void);

/**
 * Start the MQTT client task.
 * Connects to the configured broker and subscribes to topics.
 */
esp_err_t mqtt_bot_start(void);

/**
 * Stop the MQTT client.
 * Disconnects from broker and cleans up resources.
 */
esp_err_t mqtt_bot_stop(void);

/**
 * Send a text message to an MQTT topic.
 * A trailing "/request" suffix is mapped to "/response".
 * @param topic    MQTT topic to publish to
 * @param text     Message text to publish
 */
esp_err_t mqtt_send_message(const char *topic, const char *text);

/**
 * Save MQTT configuration to NVS.
 * @param broker_uri  MQTT broker URI (e.g. "mqtts://broker.example.com:8883")
 * @param client_id   MQTT client ID (optional, will auto-generate if empty)
 * @param username    MQTT username (optional)
 * @param password    MQTT password (optional)
 */
esp_err_t mqtt_set_config(const char *broker_uri, const char *client_id,
                          const char *username, const char *password);

/**
 * Set the subscribe topic pattern.
 * @param topic_pattern  Topic pattern to subscribe (supports + and # wildcards)
 */
esp_err_t mqtt_set_subscribe_topic(const char *topic_pattern);

/**
 * Check if MQTT client is connected.
 */
bool mqtt_is_connected(void);
