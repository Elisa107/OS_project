#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "common.h"
#include "signal_utils.h"
#include "errors.h"

typedef struct {
    int  id;
    int  child_id;  // controlled device (-1 if none)
    int  parent_id;
    int  begin_min; // activation time in minutes past midnight (-1 = unset)
    int  end_min; // deactivation time
    char sw_label[32]; // switch used to turn the child on/off (default "power")
    char expected[64]; // child's state after the Timer's last action ("" = unknown)
} Timer;

static void timer_init(Timer *t) {
    t->id = -1;
    t->child_id = -1; t->parent_id = -1;
    t->begin_min = -1; t->end_min = -1;
    strcpy(t->sw_label, "power");
    t->expected[0] = '\0';
}

static void extract_state(const char *payload, char *out, size_t n) {
    const char *p = strstr(payload, "state=");
    if (!p){ 
        snprintf(out, n, "?"); 
        return; 
    }
    p += 6;
    size_t i = 0;
    while(p[i] && p[i] != ' ' && i < n - 1){ 
        out[i] = p[i]; 
        i++; 
    }
    out[i] = '\0';
}

// converts "HH:MM" to minutes past midnight; -1 if the format is invalid
static int parse_hhmm(const char *s) {
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2){
        return -1;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59){
        return -1;
    }
    return h * 60 + m;
}
// reject a 'begin' time that has already passed today
static int current_minutes_now(void) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    return lt->tm_hour * 60 + lt->tm_min;
}

static int timer_send_child(int child_id, const char *payload, int sender) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1){
        return -1;
    }
    Message req; memset(&req, 0, sizeof req);
    req.command = CMD_SWITCH; req.sender = sender; req.receiver = child_id;
    strncpy(req.payload, payload, sizeof req.payload - 1);
    if (send_message(fd, &req) != 0){ 
        close_connection(fd); 
        return -1; 
    }
    Message resp;
    if (receive_message(fd, &resp) != 0){ 
        close_connection(fd); 
        return -1; 
    }
    close_connection(fd);
    return (resp.command == CMD_ACK) ? 0 : -1;
}

static int timer_query_child(int child_id, char *state_out, size_t n) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1){
        return -1;
    }
    Message req; memset(&req, 0, sizeof req);
    req.command = CMD_INFO; req.sender = -1; req.receiver = child_id;
    if (send_message(fd, &req) != 0){ 
        close_connection(fd); 
        return -1; 
    }
    Message resp;
    if (receive_message(fd, &resp) != 0){ 
        close_connection(fd); 
        return -1; 
    }
    close_connection(fd);
    extract_state(resp.payload, state_out, n);
    return 0;
}

// child's switch "vocabulary" (same as the Hub)
typedef enum { 
    VOCAB_UNKNOWN, 
    VOCAB_POWER, 
    VOCAB_OPENCLOSE 
} Vocab;

static Vocab timer_child_vocab(int child_id){
    char st[64];
    if (timer_query_child(child_id, st, sizeof st) != 0){
        return VOCAB_UNKNOWN;
    }
    if (strcmp(st, "on") == 0 || strcmp(st, "off") == 0){
        return VOCAB_POWER;
    }
    if (strcmp(st, "open") == 0 || strcmp(st, "closed") == 0){
        return VOCAB_OPENCLOSE;
    }
    return VOCAB_UNKNOWN;
}

// runs the scheduled action. translates the switch into the child's vocabulary
static void timer_actuate(Timer *t, int on) {
    if (t->child_id < 0){
        return;
    }
    char p[64];
    if (timer_child_vocab(t->child_id) == VOCAB_OPENCLOSE){
        snprintf(p, sizeof p, on ? "open:1" : "close:1");
    } else{
        snprintf(p, sizeof p, "%s:%d", t->sw_label, on ? 1 : 0);
    }
    // sender = t->id (not -1): automatic action from the timer's own
    // schedule, not a manual override
    if (timer_send_child(t->child_id, p, t->id) == 0){
        char st[64];
        if (timer_query_child(t->child_id, st, sizeof st) == 0){
            strncpy(t->expected, st, sizeof t->expected - 1);
        }
    }
}

// decides the next scheduled action. wait_sec is set to the seconds remaining until that time
static int timer_next_action(const Timer *t, int now_min, int now_sec, long *wait_sec){
    if (t->begin_min < 0 || t->end_min < 0){
        return -1;
    }
    if (now_min < t->begin_min){
        *wait_sec = (long)(t->begin_min - now_min) * 60 - now_sec;
        if (*wait_sec < 0){
            *wait_sec = 0;
        }
        return 1;
    }
    if (now_min < t->end_min) {
        *wait_sec = (long)(t->end_min - now_min) * 60 - now_sec;
        if (*wait_sec < 0){
            *wait_sec = 0;
        }
        return 0;
    }
    return -1;
}

static void fmt_hhmm(int min, char *out, size_t n){
    if (min < 0){
        snprintf(out, n, "--:--");
    } else {
        snprintf(out, n, "%02d:%02d", min / 60, min % 60);
    }
}

static void handle_message(Timer *t, const Message *in, Message *out){
    memset(out, 0, sizeof *out);
    out->sender = in->receiver;
    out->receiver = in->sender;
    out->command = CMD_ACK;

    switch (in->command) {
        // read: mirrors the child's state (or "manual override" if it differs from the state the Timer last set)
        case CMD_INFO: {
            char b[8], e[8];
            fmt_hhmm(t->begin_min, b, sizeof b);
            fmt_hhmm(t->end_min, e, sizeof e);
            if (t->child_id < 0){
                snprintf(out->payload, sizeof out->payload, "state=empty begin=%s end=%s", b, e);
                break;
            }
            char st[64];
            if (timer_query_child(t->child_id, st, sizeof st) != 0){
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s: child %d",
                        error_to_string(DEVICE_NOT_FOUND), t->child_id);
            } else if (t->expected[0] && strcmp(st, t->expected) != 0){
                snprintf(out->payload, sizeof out->payload, "state=manual override begin=%s end=%s", b, e);
            } else {
                snprintf(out->payload, sizeof out->payload, "state=%s begin=%s end=%s", st, b, e);
            }
            break;
        }

        case CMD_SWITCH: {
            char label[32] = {0}, val[64] = {0};
            if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 2) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
                break;
            }
            if (strcmp(label, "begin") == 0 || strcmp(label, "end") == 0){
                int mm = parse_hhmm(val);
                if (mm < 0){
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s: bad time format", error_to_string(INVALID_ARGUMENT));
                } else if (mm < current_minutes_now()){
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s: time already past", error_to_string(INVALID_ARGUMENT));
                } else if (strcmp(label, "begin") == 0){
                    if (t->end_min >= 0 && mm >= t->end_min) {
                        out->command = CMD_ERROR;
                        snprintf(out->payload, sizeof out->payload, "%s: begin must precede end", error_to_string(INVALID_ARGUMENT));
                    } else {
                        t->begin_min = mm;
                        snprintf(out->payload, sizeof out->payload, "begin=%s", val);
                    }
                } else {
                    if (t->begin_min >= 0 && mm <= t->begin_min){
                        out->command = CMD_ERROR;
                        snprintf(out->payload, sizeof out->payload, "%s: end must follow begin", error_to_string(INVALID_ARGUMENT));
                    } else {
                        t->end_min = mm;
                        snprintf(out->payload, sizeof out->payload, "end=%s", val);
                    }
                }
            } else if (strcmp(label, "switch") == 0){
                strncpy(t->sw_label, val, sizeof t->sw_label - 1);
                snprintf(out->payload, sizeof out->payload, "switch=%s", t->sw_label);
            } else {
                // regular switch -> propagate to the child (write mode)
                if (t->child_id < 0){
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s", error_to_string(NO_CHILDREN));
                } else if (timer_send_child(t->child_id, in->payload, in->sender) != 0){
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "%s: child %d",
                            error_to_string(DEVICE_NOT_FOUND), t->child_id);
                } else{
                    char st[64];
                    if (timer_query_child(t->child_id, st, sizeof st) == 0){
                        strncpy(t->expected, st, sizeof t->expected - 1);
                    }
                    snprintf(out->payload, sizeof out->payload, "propagated to child");
                }
            }
            break;
        }

        case CMD_LINK: {
            char label[32] = {0}, val[64] = {0};
            if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 2){
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            } else if (strcmp(label, "child") == 0){
                t->child_id = atoi(val); // timer controls a single child
                t->expected[0] = '\0';
                snprintf(out->payload, sizeof out->payload, "child=%d", t->child_id);
            } else if (strcmp(label, "parent") == 0){
                t->parent_id = atoi(val);
                snprintf(out->payload, sizeof out->payload, "parent=%d", t->parent_id);
            } else {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            }
            break;
        }

        default:
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "%s", error_to_string(INVALID_COMMAND));
            break;
    }
}

// select() waits for a command, but with a timeout when a schedule is set:
// the time left until the next action.
// on timeout, the Timer actuates the child
void timer_run(int srv_fd, int id) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof(path), "/tmp/domotic_%d.sock", id);
    register_cleanup_handler(path);

    Timer t;
    timer_init(&t);
    t.id = id;
    srand((unsigned)getpid());
    fprintf(stderr, "[timer %d] launched (pid=%d)\n", id, getpid());

    while (1){
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        int now_min = lt->tm_hour * 60 + lt->tm_min;
        int now_sec = lt->tm_sec;

        long wait_sec = 0;
        int action = timer_next_action(&t, now_min, now_sec, &wait_sec);

        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv_fd, &rset);
        struct timeval tv, *tvp = NULL;
        if (action != -1) { 
            tv.tv_sec = wait_sec; 
            tv.tv_usec = 0; 
            tvp = &tv; 
        }

        int ready = select(srv_fd + 1, &rset, NULL, NULL, tvp);
        if (ready == -1){
            if (errno == EINTR){
                continue;
            }
            perror("select");
            continue;
        }
        if (ready == 0){
            timer_actuate(&t, action == 1);
            fprintf(stderr, "[timer %d] child %s\n", id, action == 1 ? "turned on" : "turned off");
            continue;
        }

        int client = accept_connection(srv_fd);
        if (client == -1){
            continue;
        }
        Message in;
        if (receive_message(client, &in) == 0){
            sleep(1 + rand() % 3);
            Message out;
            handle_message(&t, &in, &out);
            send_message(client, &out);
            notify_controller_override(id, &in, &out);
        }
        close_connection(client);
    }
}