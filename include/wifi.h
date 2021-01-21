#ifndef _WIFI_H_
#define _WIFI_H_

#define ESP_WIFI_SSID      "PruebaTBA"
#define ESP_WIFI_PASS      "pruebaTBA"
#define ESP_MAXIMUM_RETRY  5

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define CONNECT_BIT     BIT0
#define STOP_BIT        BIT1
#define GOT_DATA_BIT    BIT2

#endif