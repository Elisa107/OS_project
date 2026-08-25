#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "common.h"
#include "signal_utils.h"
#include "errors.h"

typedef enum { 
    OFF = 0, 
    ON = 1 
} BulbState;

typedef struct {
    BulbState state;
    long    total_time;  // seconds accumulated during previous start-ups
    time_t  on_since;    // current power-on status (0 if switched off)
    int     parent_id;   // logical parent ID, -1 if not linked
} Bulb;

static void bulb_init(Bulb *b) {
    b->state = OFF;
    b->total_time = 0;
    b->on_since = 0;
    b->parent_id = -1;
}

// total_time on to be displayed in INFO: the sum of previous sessions
// plus, if the Bulb is currently on, the time elapsed since it was last switched on
static long bulb_current_time(const Bulb *b) {
    long t = b->total_time;
    if (b->state == ON) {
        t += (long)(time(NULL) - b->on_since);
    }
    return t;
}

static int parse_bit(const char *s, int *out) {
    if (s[0] != '\0' && (s[0] == '0' || s[0] == '1') && s[1] == '\0') {
        *out = s[0] - '0';
        return 1;
    }
    return 0;
}


// CMD_SWITCH payload (format "label:pos")
static void handle_message(Bulb *b, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender   = in->receiver;
    out->receiver = in->sender;
    out->command  = CMD_ACK;

    switch (in->command) {
        case CMD_INFO: {
            snprintf(out->payload, sizeof out->payload, "state=%s time=%ld", b->state == ON ? "on" : "off", bulb_current_time(b));
            break;
        }

        case CMD_SWITCH: {
            char label[32] = {0}, val[64] = {0};
            if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 1){ // label[32], val[64]. Parsing
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
                break;
            }
            if (strcmp(label, "power") != 0){   // bulb has 1 switch
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
                break;
            }

            int pos;
            if (!parse_bit(val, &pos)){  // value neither 0 nor 1
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_ARGUMENT));
                break;
            }
            if (pos == 1){  // power on
                if (b->state == OFF){
                    b->state = ON;
                    b->on_since = time(NULL);
                }
            }else{  // power off
                if (b->state == ON){
                    b->total_time += (long)(time(NULL) - b->on_since);
                    b->on_since = 0;
                }
                b->state = OFF;
            }
            snprintf(out->payload, sizeof out->payload, "state=%s", b->state == ON ? "on" : "off");
            break;
        }

        case CMD_LINK: {  //update the logic parent
            int pid = -1;
            if (sscanf(in->payload, "%d", &pid) != 1){
                pid = in->sender;
            }
            b->parent_id = pid;
            snprintf(out->payload, sizeof out->payload, "parent=%d", b->parent_id);
            break;
        }

        default:
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            break;
    }
}

void bulb_run(int srv_fd, int id) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof(path), "/tmp/domotic_%d.sock", id);
    register_cleanup_handler(path);

    Bulb b;
    bulb_init(&b);
    srand((unsigned)getpid());
    fprintf(stderr, "[bulb %d] launched (pid=%d)\n\n", id, getpid());

    while (1) {
        int client = accept_connection(srv_fd);
        if (client == -1){
            continue;
        }

        Message in;
        if (receive_message(client, &in) == 0){
            sleep(1 + rand() % 3);
            Message out;
            handle_message(&b, &in, &out);
            send_message(client, &out);
            notify_controller_override(id, &in, &out);
        }
        close_connection(client);
    }
}