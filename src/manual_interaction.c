#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "common.h"

int main(int argc, char *argv[]){
    if (argc < 3){
        fprintf(stderr,
            "Use:\n"
            "  %s <id> info\n"
            "  %s <id> switch <label> <on|off>\n"
            "  %s <id> set <param> <value>\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    int id = atoi(argv[1]);
    char *cmd = argv[2];

    Message m;
    memset(&m, 0, sizeof m);
    m.sender   = -1;   // -1 = manual (controller uses 0)
    m.receiver = id;

    if (strcmp(cmd, "info") == 0){
        m.command = CMD_INFO;
    } else if (strcmp(cmd, "switch") == 0){
        if (argc < 5) { 
            fprintf(stderr, "switch requires <label> <on|off>\n"); 
            return 1; 
        }
        int pos = (strcmp(argv[4], "on") == 0) ? 1 : 0;
        m.command = CMD_SWITCH;
        snprintf(m.payload, sizeof m.payload, "%s:%d", argv[3], pos);
    } else if (strcmp(cmd, "set") == 0){
        if(argc < 5){ 
            fprintf(stderr, "set requires <param> <value>\n"); 
            return 1; 
        }
        m.command = CMD_SWITCH;
        snprintf(m.payload, sizeof m.payload, "%s:%s", argv[3], argv[4]);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }

    char path[SOCKET_PATH_LEN];
    snprintf(path, SOCKET_PATH_LEN, "/tmp/domotic_%d.sock", id);

    int fd = connect_device(path);
    if (fd < 0) {
        fprintf(stderr, "Unable to connect to the device %d (%s)\n", id, path);
        return 1;
    }

    if (send_message(fd, &m) != 0) {
        fprintf(stderr, "Failed to send\n");
        close_connection(fd);
        return 1;
    }

    Message r;
    if (receive_message(fd, &r) != 0) {
        fprintf(stderr, "No response from the device\n");
        close_connection(fd);
        return 1;
    }
    close_connection(fd);

    printf("%s: %s\n", r.command == CMD_ACK ? "OK" : "ERR", r.payload);
    return 0;
}