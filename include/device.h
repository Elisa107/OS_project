#ifndef DEVICE_H
#define DEVICE_H
#include <sys/types.h>

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

typedef enum { 
    OFF, 
    ON, 
    PARTIAL 
} DeviceOnState;

typedef struct{
    int id;
    pid_t pid;
    DeviceType type;
    int parent_id; // -1 if it's not linked to any device
    char socket_path[108]; // 108 is the maximum length for a UNIX domain socket path
} Device;

typedef struct {
    char label[32];
    int pos;
} Switch;

typedef struct {
    Switch switches[MAX_SWITCHES];
    int switch_count;
    DeviceOnState state;   
} DeviceState;

int is_control_device(DeviceType type);
#endif