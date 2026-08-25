#include "../include/errors.h"

const char* error_to_string(int error_code) {
    switch (error_code) {
        case SUCCESS: return "Successful operation";
        case DEVICE_NOT_FOUND: return "Device not found";
        case INVALID_COMMAND: return "Invalid command";
        case LINK_FAILED: return "Connection failed";
        case DEVICE_TYPE_MISMATCH: return "Device type not compatible";
        case IPC_ERROR: return "Communication error with the device";
        case DEVICE_ALREADY_EXISTS: return "Device already exists";
        case INVALID_ARGUMENT: return "Invalid argument";
        case TOO_MANY_SWITCHES: return "Too many switches for this device";
        case NO_DEVICES: return "No device found";
        case NO_CHILDREN: return "No device connected";
        case SWITCH_REJECTED: return "Command rejected by the device";
        default: return "Undefined error";
    }
}