#include "mqtt_message.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mqtt_message_assembly_reset(mqtt_message_assembly_t *assembly)
{
    if (!assembly) {
        return;
    }

    free(assembly->topic);
    free(assembly->payload);
    *assembly = (mqtt_message_assembly_t){0};
}

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
    char **payload_out)
{
    if (!assembly || !topic_out || !payload_out) {
        return MQTT_MESSAGE_ASSEMBLY_ERROR;
    }

    *topic_out = NULL;
    *payload_out = NULL;

    if ((data_len > 0 && !data) ||
        total_len == 0 ||
        total_len > max_payload_len ||
        offset > total_len ||
        data_len > total_len - offset) {
        mqtt_message_assembly_reset(assembly);
        return MQTT_MESSAGE_ASSEMBLY_ERROR;
    }

    bool starts_message = offset == 0 && topic && topic_len > 0;

    if (starts_message) {
        mqtt_message_assembly_reset(assembly);

        assembly->topic = malloc(topic_len + 1);
        assembly->payload = malloc(total_len + 1);
        if (!assembly->topic || !assembly->payload) {
            mqtt_message_assembly_reset(assembly);
            return MQTT_MESSAGE_ASSEMBLY_ERROR;
        }

        memcpy(assembly->topic, topic, topic_len);
        assembly->topic[topic_len] = '\0';
        assembly->total_len = total_len;
        assembly->message_id = message_id;
    } else if (!assembly->topic ||
               !assembly->payload ||
               assembly->message_id != message_id ||
               assembly->total_len != total_len ||
               assembly->received_len != offset) {
        mqtt_message_assembly_reset(assembly);
        return MQTT_MESSAGE_ASSEMBLY_ERROR;
    }

    if (offset != assembly->received_len) {
        mqtt_message_assembly_reset(assembly);
        return MQTT_MESSAGE_ASSEMBLY_ERROR;
    }

    if (data_len > 0) {
        memcpy(assembly->payload + offset, data, data_len);
    }
    assembly->received_len += data_len;

    if (assembly->received_len < assembly->total_len) {
        return MQTT_MESSAGE_ASSEMBLY_INCOMPLETE;
    }

    assembly->payload[assembly->total_len] = '\0';
    *topic_out = assembly->topic;
    *payload_out = assembly->payload;
    *assembly = (mqtt_message_assembly_t){0};
    return MQTT_MESSAGE_ASSEMBLY_COMPLETE;
}

bool mqtt_string_fits(const char *value, size_t capacity)
{
    return value && capacity > 0 && strlen(value) < capacity;
}

bool mqtt_copy_string(char *destination, size_t capacity, const char *value)
{
    if (!destination || !mqtt_string_fits(value, capacity)) {
        return false;
    }

    size_t length = strlen(value);
    memcpy(destination, value, length + 1);
    return true;
}

bool mqtt_build_response_topic(const char *topic, char *output, size_t capacity)
{
    static const char request_suffix[] = "/request";
    static const char response_suffix[] = "/response";

    if (!topic || !output || capacity == 0) {
        return false;
    }

    size_t topic_len = strlen(topic);
    size_t request_suffix_len = sizeof(request_suffix) - 1;
    int written;

    if (topic_len >= request_suffix_len &&
        strcmp(topic + topic_len - request_suffix_len, request_suffix) == 0) {
        size_t prefix_len = topic_len - request_suffix_len;
        if (prefix_len > INT_MAX) {
            return false;
        }
        written = snprintf(output, capacity, "%.*s%s",
                           (int)prefix_len, topic, response_suffix);
    } else {
        written = snprintf(output, capacity, "%s", topic);
    }

    return written >= 0 && (size_t)written < capacity;
}
