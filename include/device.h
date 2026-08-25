#ifndef DEVICE_H
#define DEVICE_H
#include <sys/types.h>
#include "common.h"

#define MAX_DEVICES 100
#define MAX_SWITCHES 10

typedef enum {
    CONTROLLER,
    HUB,
    TIMER,
    BULB,
    WINDOW,
    FRIDGE
} DeviceType;

typedef struct{
    int id;
    pid_t pid;
    DeviceType type;
    int parent_id; // -1 if it's not linked to any device
    int active; // 1 if the device is active, 0 otherwise (use it to know if the device is in the array or not)
    char socket_path[SOCKET_PATH_LEN];
} Device;

int is_control_device(DeviceType type);
DeviceType parse_type(char *name);
const char* type_to_string(DeviceType type);

#endif