#ifndef PROTOCOL_H
#define PROTOCOL_H
typedef enum{
    CMD_INFO,
    CMD_SWITCH,
    CMD_LINK,
    CMD_DELETE,
    CMD_ACK,
    CMD_ERROR,
    CMD_OVERRIDE // notifica device->controller dopo override manuale (prima i device non comunicavano con controller, solo il contrario)
} Command;

typedef struct{
    Command command;
    int sender;
    int receiver;
    char payload[256];
} Message;

#endif