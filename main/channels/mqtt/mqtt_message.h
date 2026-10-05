#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *topic;
    char *payload;
    size_t total_len;
    size_t received_len;
    int message_id;
} mqtt_message_assembly_t;

typedef enum {
    MQTT_MESSAGE_ASSEMBLY_ERROR = -1,
    MQTT_MESSAGE_ASSEMBLY_INCOMPLETE = 0,
    MQTT_MESSAGE_ASSEMBLY_COMPLETE = 1,
} mqtt_message_assembly_result_t;

/**
 * Release any partial MQTT message held by an assembly context.
 */
void mqtt_message_assembly_reset(mqtt_message_assembly_t *assembly);

/**
 * Append one MQTT payload fragment.
 *
 * A fragment at offset zero starts a new message. Continuation fragments must
 * arrive in order with the same message ID and total length. On completion,
 * ownership of topic_out and payload_out passes to the caller.
 */
mqtt_message_assembly_result_t mqtt_message_assembly_append(
    mqtt_message_assembly_t *assembly,
    int message_id,
    const char *topic,
    size_t topic_len,
    const char *data,
    size_t data_len,
    size_t offset,
    size_t total_len,
    size_t max_payload_len,
    char **topic_out,
    char **payload_out);

/**
 * Return true when value, including its terminator, fits in capacity bytes.
 */
bool mqtt_string_fits(const char *value, size_t capacity);

/**
 * Copy a string without truncation.
 */
bool mqtt_copy_string(char *destination, size_t capacity, const char *value);

/**
 * Map a trailing "/request" suffix to "/response".
 *
 * Topics without that trailing suffix are copied unchanged.
 */
bool mqtt_build_response_topic(const char *topic, char *output, size_t capacity);
