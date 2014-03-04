#include <stdlib.h>
#include <canutil/read.h>
#include <pb_encode.h>
#include "can/canread.h"
#include "config.h"
#include "util/log.h"
#include "util/timer.h"

using openxc::util::log::debug;
using openxc::pipeline::MessageClass;
using openxc::pipeline::Pipeline;
using openxc::config::getConfiguration;

namespace time = openxc::util::time;
namespace pipeline = openxc::pipeline;

const char openxc::can::read::BUS_FIELD_NAME[] = "bus";
const char openxc::can::read::ID_FIELD_NAME[] = "id";
const char openxc::can::read::DATA_FIELD_NAME[] = "data";
const char openxc::can::read::NAME_FIELD_NAME[] = "name";
const char openxc::can::read::VALUE_FIELD_NAME[] = "value";
const char openxc::can::read::EVENT_FIELD_NAME[] = "event";

const char openxc::can::read::DIAGNOSTIC_MODE_FIELD_NAME[] = "mode";
const char openxc::can::read::DIAGNOSTIC_PID_FIELD_NAME[] = "pid";
const char openxc::can::read::DIAGNOSTIC_SUCCESS_FIELD_NAME[] = "success";
const char openxc::can::read::DIAGNOSTIC_NRC_FIELD_NAME[] = "negative_response_code";
const char openxc::can::read::DIAGNOSTIC_PAYLOAD_FIELD_NAME[] = "payload";
const char openxc::can::read::DIAGNOSTIC_VALUE_FIELD_NAME[] = "value";

void openxc::can::read::sendJSON(cJSON* root, Pipeline* pipeline,
        MessageClass messageClass) {
    if(root == NULL) {
        debug("JSON object is NULL -- probably OOM");
    } else {
        char* message = cJSON_PrintUnformatted(root);
        char messageWithDelimeter[strlen(message) + 3];
        strncpy(messageWithDelimeter, message, strlen(message));
        messageWithDelimeter[strlen(message)] = '\0';
        strncat(messageWithDelimeter, "\r\n", 2);

        if(message != NULL) {
            pipeline::sendMessage(pipeline, (uint8_t*) messageWithDelimeter,
                    strlen(messageWithDelimeter), messageClass);
        } else {
            debug("Converting JSON to string failed -- probably OOM");
        }
        cJSON_Delete(root);
        free(message);
    }
}

/* Private: Serialize the object to a string/protobuf
 * and send it to the pipeline.
 *
 * message - The message to send, in a struct.
 * pipeline - The pipeline to send on.
 * messageClass - the class of the message, used to decide which endpoints in
 *      the pipeline receive the message.
 */
void sendProtobuf(openxc_VehicleMessage* message, Pipeline* pipeline) {
    if(message == NULL) {
        debug("Message object is NULL");
        return;
    }
    uint8_t buffer[openxc_VehicleMessage_size + 1];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    bool status = true;
    status = pb_encode_delimited(&stream, openxc_VehicleMessage_fields,
            message);
    if(status) {
        MessageClass messageClass;
        switch(message->type) {
            case openxc_VehicleMessage_Type_TRANSLATED:
                messageClass = MessageClass::TRANSLATED;
                break;
            case openxc_VehicleMessage_Type_RAW:
                messageClass = MessageClass::RAW;
                break;
            case openxc_VehicleMessage_Type_DIAGNOSTIC:
                messageClass = MessageClass::DIAGNOSTIC;
                break;
        }
        pipeline::sendMessage(pipeline, buffer, stream.bytes_written,
                messageClass);
    } else {
        debug("Error encoding protobuf: %s", PB_GET_ERROR(&stream));
    }
}

float openxc::can::read::preTranslate(CanSignal* signal, uint64_t data,
        bool* send) {
    float value = eightbyte_parse_float(data, signal->bitPosition,
            signal->bitSize, signal->factor, signal->offset);

    if(time::tick(&signal->frequencyClock) ||
            (value != signal->lastValue && signal->forceSendChanged)) {
        if(send && (!signal->received || signal->sendSame ||
                    value != signal->lastValue)) {
            signal->received = true;
        } else {
            *send = false;
        }
    } else {
        *send = false;
    }
    return value;
}

void openxc::can::read::postTranslate(CanSignal* signal, float value) {
    signal->lastValue = value;
}

float openxc::can::read::passthroughHandler(CanSignal* signal,
        CanSignal* signals, int signalCount, Pipeline* pipeline, float value,
        bool* send) {
    return value;
}

bool openxc::can::read::booleanHandler(CanSignal* signal, CanSignal* signals,
        int signalCount, Pipeline* pipeline, float value, bool* send) {
    return value == 0.0 ? false : true;
}

float openxc::can::read::ignoreHandler(CanSignal* signal, CanSignal* signals,
        int signalCount, Pipeline* pipeline, float value, bool* send) {
    *send = false;
    return value;
}

const char* openxc::can::read::stateHandler(CanSignal* signal,
        CanSignal* signals, int signalCount, Pipeline* pipeline, float value,
        bool* send) {
    const CanSignalState* signalState = lookupSignalState(value, signal,
            signals, signalCount);
    if(signalState != NULL) {
        return signalState->name;
    }
    *send = false;
    return NULL;
}

void openxc::can::read::sendNumericalMessage(const char* name, float value,
        Pipeline* pipeline) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_TRANSLATED;
    message.has_translated_message = true;
    message.translated_message = {0};
    message.translated_message.has_name = true;
    strcpy(message.translated_message.name, name);
    message.translated_message.has_type = true;
    message.translated_message.type = openxc_TranslatedMessage_Type_NUM;
    message.translated_message.has_numeric_value = true;
    message.translated_message.numeric_value = value;

    sendVehicleMessage(&message, pipeline);
}

void openxc::can::read::sendBooleanMessage(const char* name, bool value,
        Pipeline* pipeline) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_TRANSLATED;
    message.has_translated_message = true;
    message.translated_message = {0};
    message.translated_message.has_name = true;
    strcpy(message.translated_message.name, name);
    message.translated_message.has_type = true;
    message.translated_message.type = openxc_TranslatedMessage_Type_BOOL;
    message.translated_message.has_boolean_value = true;
    message.translated_message.boolean_value = value;

    sendVehicleMessage(&message, pipeline);
}

void openxc::can::read::sendStringMessage(const char* name, const char* value,
        Pipeline* pipeline) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_TRANSLATED;
    message.has_translated_message = true;
    message.translated_message = {0};
    message.translated_message.has_name = true;
    strcpy(message.translated_message.name, name);
    message.translated_message.has_type = true;
    message.translated_message.type = openxc_TranslatedMessage_Type_STRING;
    message.translated_message.has_string_value = true;
    strcpy(message.translated_message.string_value, value);

    sendVehicleMessage(&message, pipeline);
}

void openxc::can::read::sendEventedFloatMessage(const char* name,
        const char* value, float event,
        Pipeline* pipeline) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_TRANSLATED;
    message.has_translated_message = true;
    message.translated_message = {0};
    message.translated_message.has_name = true;
    strcpy(message.translated_message.name, name);
    message.translated_message.has_type = true;
    message.translated_message.type = openxc_TranslatedMessage_Type_EVENTED_NUM;
    message.translated_message.has_string_value = true;
    strcpy(message.translated_message.string_value, value);
    message.translated_message.has_numeric_event = true;
    message.translated_message.numeric_event = event;

    sendVehicleMessage(&message, pipeline);
}

void openxc::can::read::sendEventedBooleanMessage(const char* name,
        const char* value, bool event, Pipeline* pipeline) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_TRANSLATED;
    message.has_translated_message = true;
    message.translated_message = {0};
    message.translated_message.has_name = true;
    strcpy(message.translated_message.name, name);
    message.translated_message.has_type = true;
    message.translated_message.type =
            openxc_TranslatedMessage_Type_EVENTED_BOOL;
    message.translated_message.has_string_value = true;
    strcpy(message.translated_message.string_value, value);
    message.translated_message.has_boolean_event = true;
    message.translated_message.boolean_event = event;

    sendVehicleMessage(&message, pipeline);
}

void openxc::can::read::sendEventedStringMessage(const char* name,
        const char* value, const char* event, Pipeline* pipeline) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_TRANSLATED;
    message.has_translated_message = true;
    message.translated_message = {0};
    message.translated_message.has_name = true;
    strcpy(message.translated_message.name, name);
    message.translated_message.has_type = true;
    message.translated_message.type =
            openxc_TranslatedMessage_Type_EVENTED_STRING;
    message.translated_message.has_string_value = true;
    strcpy(message.translated_message.string_value, value);
    message.translated_message.has_string_event = true;
    strcpy(message.translated_message.string_event, event);

    sendVehicleMessage(&message, pipeline);
}

static void sendTranslatedJsonMessage(openxc_VehicleMessage* message,
        Pipeline* pipeline) {
    const char* name = message->translated_message.name;
    cJSON* value = NULL;
    if(message->translated_message.has_numeric_value) {
        value = cJSON_CreateNumber(
                message->translated_message.numeric_value);
    } else if(message->translated_message.has_boolean_value) {
        value = cJSON_CreateBool(
                message->translated_message.boolean_value);
    } else if(message->translated_message.has_string_value) {
        value = cJSON_CreateString(
                message->translated_message.string_value);
    }

    cJSON* event = NULL;
    if(message->translated_message.has_numeric_event) {
        event = cJSON_CreateNumber(
                message->translated_message.numeric_event);
    } else if(message->translated_message.has_boolean_event) {
        event = cJSON_CreateBool(
                message->translated_message.boolean_event);
    } else if(message->translated_message.has_string_event) {
        event = cJSON_CreateString(
                message->translated_message.string_event);
    }

    cJSON *root = cJSON_CreateObject();
    if(root != NULL) {
        cJSON_AddStringToObject(root, openxc::can::read::NAME_FIELD_NAME, name);
        cJSON_AddItemToObject(root, openxc::can::read::VALUE_FIELD_NAME, value);
        if(event != NULL) {
            cJSON_AddItemToObject(root, openxc::can::read::EVENT_FIELD_NAME,
                    event);
        }
        openxc::can::read::sendJSON(root, pipeline, MessageClass::TRANSLATED);
    } else {
        debug("Unable to allocate a cJSON object - probably OOM");
    }
}

static void sendRawJsonMessage(openxc_VehicleMessage* message, Pipeline* pipeline) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, openxc::can::read::BUS_FIELD_NAME,
            message->raw_message.bus);
    cJSON_AddNumberToObject(root, openxc::can::read::ID_FIELD_NAME,
            message->raw_message.message_id);

    char encodedData[67];
    const char* maxAddress = encodedData + sizeof(encodedData);
    char* encodedDataIndex = encodedData;
    encodedDataIndex += sprintf(encodedDataIndex, "0x");
    for(uint8_t i = 0; i < message->raw_message.data.size &&
            encodedDataIndex < maxAddress; i++) {
        encodedDataIndex += snprintf(encodedDataIndex,
                maxAddress - encodedDataIndex,
                "%02x", message->raw_message.data.bytes[i]);
    }
    cJSON_AddStringToObject(root, openxc::can::read::DATA_FIELD_NAME,
            encodedData);

    openxc::can::read::sendJSON(root, pipeline, MessageClass::RAW);
}

static void sendDiagnosticJsonMessage(openxc_VehicleMessage* message,
        Pipeline* pipeline) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, openxc::can::read::BUS_FIELD_NAME,
            message->diagnostic_response.bus);
    cJSON_AddNumberToObject(root, openxc::can::read::ID_FIELD_NAME,
            message->diagnostic_response.message_id);
    cJSON_AddNumberToObject(root, openxc::can::read::DIAGNOSTIC_MODE_FIELD_NAME,
            message->diagnostic_response.mode);
    cJSON_AddBoolToObject(root, openxc::can::read::DIAGNOSTIC_SUCCESS_FIELD_NAME,
            message->diagnostic_response.success);

    if(message->diagnostic_response.has_pid) {
        cJSON_AddNumberToObject(root, openxc::can::read::DIAGNOSTIC_PID_FIELD_NAME,
                message->diagnostic_response.pid);
    }

    if(message->diagnostic_response.has_negative_response_code) {
        cJSON_AddNumberToObject(root, openxc::can::read::DIAGNOSTIC_NRC_FIELD_NAME,
                message->diagnostic_response.negative_response_code);
    }


    if(message->diagnostic_response.has_value) {
        cJSON_AddNumberToObject(root, openxc::can::read::DIAGNOSTIC_VALUE_FIELD_NAME,
                message->diagnostic_response.value);
    } else if(message->diagnostic_response.has_payload) {
        char encodedData[67];
        const char* maxAddress = encodedData + sizeof(encodedData);
        char* encodedDataIndex = encodedData;
        encodedDataIndex += sprintf(encodedDataIndex, "0x");
        for(uint8_t i = 0; i < message->diagnostic_response.payload.size &&
                encodedDataIndex < maxAddress; i++) {
            encodedDataIndex += snprintf(encodedDataIndex,
                    maxAddress - encodedDataIndex,
                    "%02x",
                    message->diagnostic_response.payload.bytes[i]);
        }
        cJSON_AddStringToObject(root, openxc::can::read::DIAGNOSTIC_PAYLOAD_FIELD_NAME,
                encodedData);
    }

    openxc::can::read::sendJSON(root, pipeline, MessageClass::DIAGNOSTIC);
}

/* Private: Encode the given message data into a JSON object (conforming to
 * the OpenXC standard) and send it out to the pipeline.
 *
 * This will accept both raw and translated typed messages.
 *
 * message - A message structure containing the type and data for the message.
 * pipeline - The pipeline to send on.
 */
void sendJsonMessage(openxc_VehicleMessage* message, Pipeline* pipeline) {
    if(message->type == openxc_VehicleMessage_Type_TRANSLATED) {
        sendTranslatedJsonMessage(message, pipeline);
    } else if(message->type == openxc_VehicleMessage_Type_RAW) {
        sendRawJsonMessage(message, pipeline);
    } else if(message->type == openxc_VehicleMessage_Type_DIAGNOSTIC) {
        sendDiagnosticJsonMessage(message, pipeline);
    } else {
        debug("Unrecognized message type -- not sending");
    }
}

void openxc::can::read::sendVehicleMessage(openxc_VehicleMessage* message,
        Pipeline* pipeline) {
    if(getConfiguration()->outputFormat == openxc::config::PROTO) {
        sendProtobuf(message, pipeline);
    } else {
        sendJsonMessage(message, pipeline);
    }
}

void openxc::can::read::passthroughMessage(CanBus* bus, CanMessage* message,
        CanMessageDefinition* messages, int messageCount, Pipeline* pipeline) {
    bool send = true;
    CanMessageDefinition* messageDefinition = lookupMessageDefinition(bus,
            message->id, messages, messageCount);
    if(messageDefinition == NULL) {
        debug("Adding new message definition for message %d on bus %d",
                message->id, bus->address);
        send = registerMessageDefinition(bus, message->id, messages, messageCount);
    } else if(time::tick(&messageDefinition->frequencyClock) ||
            (message->data != messageDefinition->lastValue &&
                 messageDefinition->forceSendChanged)) {
        send = true;
    } else {
        send = false;
    }

    if(send) {
        openxc_VehicleMessage vehicleMessage = {0};
        vehicleMessage.has_type = true;
        vehicleMessage.type = openxc_VehicleMessage_Type_RAW;
        vehicleMessage.has_raw_message = true;
        vehicleMessage.raw_message = {0};
        vehicleMessage.raw_message.has_message_id = true;
        vehicleMessage.raw_message.message_id = message->id;
        vehicleMessage.raw_message.has_bus = true;
        vehicleMessage.raw_message.bus = bus->address;
        vehicleMessage.raw_message.has_data = true;
        vehicleMessage.raw_message.data.size = message->length;

        union {
            uint64_t whole;
            uint8_t bytes[8];
        } combined;
        combined.whole = message->data;
        memcpy(vehicleMessage.raw_message.data.bytes, combined.bytes,
                message->length);

        sendVehicleMessage(&vehicleMessage, pipeline);
    }

    if(messageDefinition != NULL) {
        messageDefinition->lastValue = message->data;
    }
}

void openxc::can::read::translateSignal(Pipeline* pipeline, CanSignal* signal,
        uint64_t data, NumericalHandler handler, CanSignal* signals,
        int signalCount) {
    bool send = true;
    float value = preTranslate(signal, data, &send);
    float processedValue = handler(signal, signals, signalCount, pipeline,
            value, &send);
    if(send) {
        sendNumericalMessage(signal->genericName, processedValue, pipeline);
    }
    postTranslate(signal, value);
}

void openxc::can::read::translateSignal(Pipeline* pipeline, CanSignal* signal,
        uint64_t data, StringHandler handler, CanSignal* signals,
        int signalCount) {
    bool send = true;
    float value = preTranslate(signal, data, &send);
    const char* stringValue = handler(signal, signals, signalCount, pipeline,
            value, &send);
    if(stringValue != NULL && send) {
        sendStringMessage(signal->genericName, stringValue, pipeline);
    }
    postTranslate(signal, value);
}

void openxc::can::read::translateSignal(Pipeline* pipeline, CanSignal* signal,
        uint64_t data, BooleanHandler handler, CanSignal* signals,
        int signalCount) {
    bool send = true;
    float value = preTranslate(signal, data, &send);
    bool booleanValue = handler(signal, signals, signalCount, pipeline, value,
            &send);
    if(send) {
        sendBooleanMessage(signal->genericName, booleanValue, pipeline);
    }
    postTranslate(signal, value);
}

void openxc::can::read::translateSignal(Pipeline* pipeline, CanSignal* signal,
        uint64_t data, CanSignal* signals, int signalCount) {
    translateSignal(pipeline, signal, data, passthroughHandler, signals,
            signalCount);
}
