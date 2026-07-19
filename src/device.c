#include <string.h>

#include "device.h"

int is_control_device(DeviceType type) {
    return (type == CONTROLLER || type == HUB || type == TIMER); // 1 if it's a control device, 0 otherwise
}

const char* type_to_string(DeviceType type) {
    switch (type) {
        case HUB: return "HUB";
        case TIMER: return "TIMER";
        case BULB: return "BULB";
        case WINDOW: return "WINDOW";
        case FRIDGE: return "FRIDGE";
        default: return "UNKNOWN";
    }
}

DeviceType parse_type(char *name) {
    if (strcmp(name, "hub") == 0) return HUB;
    if (strcmp(name, "timer") == 0) return TIMER;
    if (strcmp(name, "bulb") == 0) return BULB;
    if (strcmp(name, "window") == 0) return WINDOW;
    if (strcmp(name, "fridge") == 0) return FRIDGE;
    return -1;
}