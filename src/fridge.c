#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "signal_utils.h"
#include "common.h"
#include "errors.h"

typedef enum { 
    CLOSED = 0, 
    OPEN = 1 
} FridgeState;

typedef struct {
    FridgeState state;
    int    delay; // after this time the fridge closes itself
    int    perc;  // fill level 0–100, via manual interaction only 
    double temp;
    double thermostat; // via manual_interaction only
    int    parent_id;
    time_t open_since; // opening time (0 if closed)
    time_t open_deadline; // the moment the auto-lock is triggered
} Fridge;

static void fridge_init(Fridge *f) {
    f->state = CLOSED; f->delay = 10;
    f->perc = 0; f->temp = 4.0; f->thermostat = 4.0; f->parent_id = -1;
    f->open_since = 0; f->open_deadline = 0;
}

// in out the response to the command in in
// perc, thermostat, delay only via manual_interaction
static void handle_message(Fridge *f, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender   = in->receiver;
    out->receiver = in->sender;
    out->command  = CMD_ACK;

    switch (in->command){
        case CMD_INFO: {
            int t = (f->state == OPEN) ? (int)(time(NULL) - f->open_since) : 0;
            snprintf(out->payload, sizeof out->payload,
                "state=%s time=%d delay=%d perc=%d temp=%.1f thermostat=%.1f",
                f->state == OPEN ? "open" : "closed",
                t, f->delay, f->perc, f->temp, f->thermostat);
            break;
        }

        case CMD_SWITCH: {
            char label[32] = {0}, val[64] = {0};
            if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 1) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            } else if(strcmp(label, "open") == 0){
                if (atoi(val) != 0){  //opening, restart timer
                    f->state = OPEN;
                    f->open_since = time(NULL);
                    f->open_deadline = f->open_since + f->delay;
                }
                snprintf(out->payload, sizeof out->payload, "state=%s", f->state==OPEN?"open":"closed");
            } else if (strcmp(label, "close") == 0){
                if (atoi(val) != 0) {
                    f->state = CLOSED;
                    f->open_since = 0;
                    f->open_deadline = 0;
                }
                snprintf(out->payload, sizeof out->payload, "state=%s", f->state==OPEN?"open":"closed");
            } else if (strcmp(label, "perc") == 0){
                if (in->sender != -1){  // only via manual_interaction
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s: perc", error_to_string(SWITCH_REJECTED));
                    break;
                }
                int v = atoi(val);
                if(v < 0 || v > 100){
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_ARGUMENT));
                }else{
                    f->perc = v;
                    snprintf(out->payload, sizeof out->payload, "perc=%d", f->perc);
                }
            } else if(strcmp(label, "thermostat") == 0) {
                if(in->sender != -1) {  // only via manual interaction
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s: thermostat", error_to_string(SWITCH_REJECTED));
                    break;
                }
                f->thermostat = atof(val);
                snprintf(out->payload, sizeof out->payload, "thermostat=%.1f", f->thermostat);
            } else if(strcmp(label, "delay") == 0){
                int d = atoi(val);
                if(d < 0) {
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_ARGUMENT));
                } else {
                    f->delay = d;
                    if (f->state == OPEN) f->open_deadline = f->open_since + f->delay;
                    snprintf(out->payload, sizeof out->payload, "delay=%d", f->delay);
                }
            } else {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            }
            break;
        }

        case CMD_LINK: { // set logic parent
            int pid = -1;
            if (sscanf(in->payload, "%d", &pid) != 1) pid = in->sender;
            f->parent_id = pid;
            snprintf(out->payload, sizeof out->payload, "parent=%d", f->parent_id);
            break;
        }

        default:
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            break;
    }
}

// One connection handled per loop iteration. select() waits for the
// next command, but with a timeout while the fridge is open (if none
// arrives before the deadline, it self-closes)
void fridge_run(int srv_fd, int id) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof(path), "/tmp/domotic_%d.sock", id);
    register_cleanup_handler(path);

    Fridge f;
    fridge_init(&f);
    srand((unsigned)getpid());
    fprintf(stderr, "[fridge %d] launched (pid=%d)\n", id, getpid());

    while (1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv_fd, &rset);

        struct timeval tv, *tvp;
        if (f.state == OPEN) {
            long remaining = (long)(f.open_deadline - time(NULL));
            if (remaining < 0) remaining = 0;
            tv.tv_sec = remaining;
            tv.tv_usec = 0;
            tvp = &tv;
        } else {
            tvp = NULL;
        }

        int ready = select(srv_fd + 1, &rset, NULL, NULL, tvp);

        if(ready == -1){
            if (errno == EINTR){
                continue;
            }
            perror("select");
            continue;
        }
        if (ready == 0){
            f.state = CLOSED;
            f.open_since = 0;
            f.open_deadline = 0;
            fprintf(stderr, "[fridge %d] self-closing (delay %d s expired)\n", id, f.delay);
            continue;
        }

        int client = accept_connection(srv_fd);
        if (client == -1){
            continue;
        }

        Message in;
        if(receive_message(client, &in) == 0){
            sleep(1 + rand() % 3);
            Message out;
            handle_message(&f, &in, &out);
            send_message(client, &out);
            notify_controller_override(id, &in, &out);
        }
        close_connection(client);
    }
}