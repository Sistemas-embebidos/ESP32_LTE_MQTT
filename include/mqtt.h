#ifndef _MQTT_H_
#define _MQTT_H_

#define BROKER_USER "f8caa162"
#define BROKER_PASS "4dfa0cbff7b885ea"
#define BROKER_HOST "broker.shiftr.io"

//#define BROKER_URL "mqtt://f8caa162:4dfa0cbff7b885ea@broker.shiftr.io"
#define BROKER_URL "mqtt://" BROKER_USER ":" BROKER_PASS "@" BROKER_HOST

//#define NAME "ESP32_MARTIN"

#define STATE_TOPIC "/states"
#define DATA_TOPIC "/data"
#define CONFIG_TOPIC "/configuration"

#endif