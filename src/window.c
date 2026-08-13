#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "common.h"
#include "signal_utils.h"

typedef enum { CLOSED = 0, OPEN = 1 } WindowState;

typedef struct {
    WindowState state;
    long    total_time;  /* secondi cumulati trascorsi aperta (sessioni chiuse) */
    time_t  open_since;  /* istante dell'apertura corrente (0 se chiusa) */
    int     parent_id;
} Window;

static void window_init(Window *w) {
    w->state = CLOSED;
    w->total_time = 0;
    w->open_since = 0;
    w->parent_id = -1;
}

static long window_current_time(const Window *w) {
    long t = w->total_time;
    if (w->state == OPEN) {
        t += (long)(time(NULL) - w->open_since);
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


static void handle_message(Window *w, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender   = in->receiver;
    out->receiver = in->sender;
    out->command  = CMD_ACK;

    switch (in->command) {

    case CMD_INFO: {
        snprintf(out->payload, sizeof out->payload,
            "state=%s time=%ld",
            w->state == OPEN ? "open" : "closed", window_current_time(w));
        break;
    }

    case CMD_SWITCH: {
        char label[32] = {0}, val[64] = {0};
        if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 1) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
            break;
        }

        if (strcmp(label, "open") != 0 && strcmp(label, "close") != 0) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
            break;
        }

        int pos;
        if (!parse_bit(val, &pos)) {          /* valore non "0" né "1" -> errore esplicito */
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_ARGUMENT");
            break;
        }

        if (strcmp(label, "open") == 0) {
            if (pos == 1 && w->state == CLOSED) {
                w->state = OPEN;
                w->open_since = time(NULL);
            }
        } else { /* "close" */
            if (pos == 1 && w->state == OPEN) {
                w->total_time += (long)(time(NULL) - w->open_since);
                w->open_since = 0;
                w->state = CLOSED;
            }
        }
        snprintf(out->payload, sizeof out->payload,
            "state=%s", w->state == OPEN ? "open" : "closed");
        break;
    }

    case CMD_LINK: {
        int pid = -1;
        if (sscanf(in->payload, "%d", &pid) != 1) pid = in->sender;
        w->parent_id = pid;
        snprintf(out->payload, sizeof out->payload, "parent=%d", w->parent_id);
        break;
    }

    default:
        out->command = CMD_ERROR;
        snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        break;
    }
}

void window_run(int srv_fd, int id) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof(path), "/tmp/domotic_%d.sock", id);
    register_cleanup_handler(path);

    Window w;
    window_init(&w);
    srand((unsigned)getpid());
    fprintf(stderr, "[window %d] avviato (pid=%d)\n\n", id, getpid());

    while (1) {
        int client = accept_connection(srv_fd);
        if (client == -1) continue;

        Message in;
        if (receive_message(client, &in) == 0) {
            sleep(1 + rand() % 3);
            Message out;
            handle_message(&w, &in, &out);
            send_message(client, &out);
            notify_controller_override(id, &in, &out);
        }
        close_connection(client);
    }
}