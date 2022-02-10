#ifndef MQTT_BENCH_H
#define MQTT_BENCH_H

#define APP_NAME "nng-mqtt-bench"

enum client_type
{
    PUB,
    SUB,
    CONN
};

void fatal(const char *msg, ...);
void client(int argc, char **argv, enum client_type type);

#endif
