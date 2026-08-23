#include "channels/mqtt/mqtt_message.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void test_single_fragment(void)
{
    mqtt_message_assembly_t assembly = {0};
    char *topic = NULL;
    char *payload = NULL;

    mqtt_message_assembly_result_t result = mqtt_message_assembly_append(
        &assembly, 10, "chat/request", 12, "hello", 5, 0, 5, 64,
        &topic, &payload);

    assert(result == MQTT_MESSAGE_ASSEMBLY_COMPLETE);
    assert(strcmp(topic, "chat/request") == 0);
    assert(strcmp(payload, "hello") == 0);
    free(topic);
    free(payload);
}

static void test_repeated_message_is_valid(void)
{
    mqtt_message_assembly_t assembly = {0};

    for (int message_id = 20; message_id < 22; message_id++) {
        char *topic = NULL;
        char *payload = NULL;

        assert(mqtt_message_assembly_append(
                   &assembly, message_id, "chat/request", 12,
                   "repeat", 6, 0, 6, 64,
                   &topic, &payload) == MQTT_MESSAGE_ASSEMBLY_COMPLETE);
        assert(strcmp(topic, "chat/request") == 0);
        assert(strcmp(payload, "repeat") == 0);
        free(topic);
        free(payload);
    }
}

static void test_fragmented_payload(void)
{
    mqtt_message_assembly_t assembly = {0};
    char *topic = NULL;
    char *payload = NULL;

    mqtt_message_assembly_result_t result = mqtt_message_assembly_append(
        &assembly, 11, "chat/request", 12, "{\"text\":", 8, 0, 13, 64,
        &topic, &payload);
    assert(result == MQTT_MESSAGE_ASSEMBLY_INCOMPLETE);
    assert(topic == NULL);
    assert(payload == NULL);

    result = mqtt_message_assembly_append(
        &assembly, 11, NULL, 0, "\"hi\"}", 5, 8, 13, 64,
        &topic, &payload);
    assert(result == MQTT_MESSAGE_ASSEMBLY_COMPLETE);
    assert(strcmp(topic, "chat/request") == 0);
    assert(strcmp(payload, "{\"text\":\"hi\"}") == 0);
    free(topic);
    free(payload);
}

static void test_topic_only_first_fragment(void)
{
    mqtt_message_assembly_t assembly = {0};
    char *topic = NULL;
    char *payload = NULL;

    mqtt_message_assembly_result_t result = mqtt_message_assembly_append(
        &assembly, 14, "chat/request", 12, NULL, 0, 0, 5, 64,
        &topic, &payload);
    assert(result == MQTT_MESSAGE_ASSEMBLY_INCOMPLETE);

    result = mqtt_message_assembly_append(
        &assembly, 14, NULL, 0, "hello", 5, 0, 5, 64,
        &topic, &payload);
    assert(result == MQTT_MESSAGE_ASSEMBLY_COMPLETE);
    assert(strcmp(topic, "chat/request") == 0);
    assert(strcmp(payload, "hello") == 0);
    free(topic);
    free(payload);
}

static void test_invalid_fragment_sequence_resets(void)
{
    mqtt_message_assembly_t assembly = {0};
    char *topic = NULL;
    char *payload = NULL;

    assert(mqtt_message_assembly_append(
               &assembly, 12, "chat/request", 12, "abc", 3, 0, 6, 64,
               &topic, &payload) == MQTT_MESSAGE_ASSEMBLY_INCOMPLETE);
    assert(mqtt_message_assembly_append(
               &assembly, 12, NULL, 0, "def", 3, 4, 6, 64,
               &topic, &payload) == MQTT_MESSAGE_ASSEMBLY_ERROR);
    assert(assembly.topic == NULL);
    assert(assembly.payload == NULL);
}

static void test_oversized_payload_is_rejected(void)
{
    mqtt_message_assembly_t assembly = {0};
    char *topic = NULL;
    char *payload = NULL;

    assert(mqtt_message_assembly_append(
               &assembly, 13, "chat/request", 12, "abc", 3, 0, 65, 64,
               &topic, &payload) == MQTT_MESSAGE_ASSEMBLY_ERROR);
}

static void test_response_topic_mapping(void)
{
    char output[64];

    assert(mqtt_build_response_topic(
        "mimiclaw/chat/alice/request", output, sizeof(output)));
    assert(strcmp(output, "mimiclaw/chat/alice/response") == 0);

    assert(mqtt_build_response_topic(
        "mimiclaw/request/archive", output, sizeof(output)));
    assert(strcmp(output, "mimiclaw/request/archive") == 0);

    assert(!mqtt_build_response_topic(
        "mimiclaw/chat/alice/request", output, 12));
}

static void test_string_boundaries(void)
{
    char destination[5];

    assert(mqtt_string_fits("four", sizeof(destination)));
    assert(!mqtt_string_fits("five!", sizeof(destination)));
    assert(mqtt_copy_string(destination, sizeof(destination), "four"));
    assert(strcmp(destination, "four") == 0);
    assert(!mqtt_copy_string(destination, sizeof(destination), "five!"));
}

int main(void)
{
    test_single_fragment();
    test_repeated_message_is_valid();
    test_fragmented_payload();
    test_topic_only_first_fragment();
    test_invalid_fragment_sequence_resets();
    test_oversized_payload_is_rejected();
    test_response_topic_mapping();
    test_string_boundaries();
    return 0;
}
