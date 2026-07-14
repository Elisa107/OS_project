typedef enum{
    CMD_INFO,
    CMD_SWITCH,
    CMD_LINK,
    CMD_DELETE,
    CMD_ACK,
    CMD_ERROR
} Command;

typedef struct{
    Command command;
    int sender;
    int receiver;
    char payload[256];
} Message;