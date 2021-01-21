#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/adc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_event_loop.h"

// Wifi
#include "tcpip_adapter.h"

#include "tasks.h"
#include "mqtt.h"
#include "wifi.h"
#include "aws.h"
#include "storage.h"

#define LTE_READY false

#define WIFI_TAG "[WiFi]"


#endif