#ifndef ERRORS_H
#define ERRORS_H
// will use these in controller.c and device.c to indicate success or failure of a function
#define SUCCESS 0

#define DEVICE_NOT_FOUND -1
#define INVALID_COMMAND -2
#define LINK_FAILED -3
#define DEVICE_TYPE_MISMATCH -4
#define IPC_ERROR -5
#define DEVICE_ALREADY_EXISTS -6
#define INVALID_ARGUMENT -7
#define TOO_MANY_SWITCHES -8
#define NO_DEVICES -9
#define NO_CHILDREN -10

const char* error_to_string(int error_code);

#endif