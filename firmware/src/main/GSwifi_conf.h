#ifndef _GSWIFI_CONF_H_
#define _GSWIFI_CONF_H_

#define GS_BAUD 115200 // TODO tune up

//#define GS_UART_DIRECT
#define GS_BULK

#define GS_DNSNAME "setup.local"
#define GS_WREGDOMAIN 2 // 0:FCC, 1:ETSI, 2:TELEC

#define GS_TIMEOUT   10000 // ms
#define GS_TIMEOUT2  60000 // ms
#define GS_RECONNECT 60 // s

#define GS_CMD_SIZE 64

#define GS_MAX_ROUTES 2
#define GS_MAX_PATH_LENGTH 6

//#define GS_ENABLE_MDNS

#define HTTPD_TIMEOUT 15000

// #define HTTPD_BUF_SIZE 200
// #define HTTPD_URI_SIZE 100

#define HTTPD_KEEPALIVE 10 // request count

#endif
