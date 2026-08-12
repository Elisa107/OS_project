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

typedef struct {
    int  child_id;      /* device controllato (-1 se nessuno) */
    int  parent_id;
    int  begin_min;     /* orario di accensione in minuti dalla mezzanotte (-1 = non impostato) */
    int  end_min;       /* orario di spegnimento */
    char sw_label[32];  /* switch usato per accendere/spegnere il figlio (default "power") */
    char expected[64];  /* stato del figlio dopo l'ultima azione del Timer ("" = sconosciuto) */
} Timer;

static void timer_init(Timer *t) {
    t->child_id = -1; t->parent_id = -1;
    t->begin_min = -1; t->end_min = -1;
    strcpy(t->sw_label, "power");
    t->expected[0] = '\0';
}

static void extract_state(const char *payload, char *out, size_t n) {
    const char *p = strstr(payload, "state=");
    if (!p) { snprintf(out, n, "?"); return; }
    p += 6;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

/* Converte "HH:MM" in minuti dalla mezzanotte; -1 se il formato non e' valido. */
static int parse_hhmm(const char *s) {
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

static int timer_send_child(int child_id, const char *payload) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1) return -1;
    Message req; memset(&req, 0, sizeof req);
    req.command = CMD_SWITCH; req.sender = -1; req.receiver = child_id;
    strncpy(req.payload, payload, sizeof req.payload - 1);
    if (send_message(fd, &req) != 0) { close_connection(fd); return -1; }
    Message resp;
    if (receive_message(fd, &resp) != 0) { close_connection(fd); return -1; }
    close_connection(fd);
    return (resp.command == CMD_ACK) ? 0 : -1;
}

static int timer_query_child(int child_id, char *state_out, size_t n) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1) return -1;
    Message req; memset(&req, 0, sizeof req);
    req.command = CMD_INFO; req.sender = -1; req.receiver = child_id;
    if (send_message(fd, &req) != 0) { close_connection(fd); return -1; }
    Message resp;
    if (receive_message(fd, &resp) != 0) { close_connection(fd); return -1; }
    close_connection(fd);
    extract_state(resp.payload, state_out, n);
    return 0;
}

/* Esegue l'azione oraria: on=1 accende il figlio, on=0 lo spegne. */
static void timer_actuate(Timer *t, int on) {
    if (t->child_id < 0) return;
    char p[64];
    snprintf(p, sizeof p, "%s:%d", t->sw_label, on ? 1 : 0);
    if (timer_send_child(t->child_id, p) == 0) {
        char st[64];
        if (timer_query_child(t->child_id, st, sizeof st) == 0)
            strncpy(t->expected, st, sizeof t->expected - 1);
    }
}

/* Decide la prossima azione oraria: 1 = accendi (begin), 0 = spegni (end),
 * -1 = nessuna. In wait_sec mette i secondi di attesa fino a quell'ora. */
static int timer_next_action(const Timer *t, int now_min, int now_sec, long *wait_sec) {
    if (t->begin_min < 0 || t->end_min < 0) return -1;
    if (now_min < t->begin_min) {
        *wait_sec = (long)(t->begin_min - now_min) * 60 - now_sec;
        if (*wait_sec < 0) *wait_sec = 0;
        return 1;
    }
    if (now_min < t->end_min) {
        *wait_sec = (long)(t->end_min - now_min) * 60 - now_sec;
        if (*wait_sec < 0) *wait_sec = 0;
        return 0;
    }
    return -1;
}

static void fmt_hhmm(int min, char *out, size_t n) {
    if (min < 0) snprintf(out, n, "--:--");
    else snprintf(out, n, "%02d:%02d", min / 60, min % 60);
}

static void handle_message(Timer *t, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender = in->receiver;
    out->receiver = in->sender;
    out->command = CMD_ACK;

    switch (in->command) {

    /* Lettura: rispecchia lo stato del figlio (o "manual override" se differisce
     * dallo stato che il Timer aveva impostato). Mostra anche begin/end. */
    case CMD_INFO: {
        char b[8], e[8];
        fmt_hhmm(t->begin_min, b, sizeof b);
        fmt_hhmm(t->end_min, e, sizeof e);
        if (t->child_id < 0) {
            snprintf(out->payload, sizeof out->payload, "state=empty begin=%s end=%s", b, e);
            break;
        }
        char st[64];
        if (timer_query_child(t->child_id, st, sizeof st) != 0) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "figlio %d non raggiungibile", t->child_id);
        } else if (t->expected[0] && strcmp(st, t->expected) != 0) {
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
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
            break;
        }
        if (strcmp(label, "begin") == 0 || strcmp(label, "end") == 0) {
            int mm = parse_hhmm(val);
            if (mm < 0) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "orario non valido");
            } else if (strcmp(label, "begin") == 0) {
                if (t->end_min >= 0 && mm >= t->end_min) {
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "begin deve precedere end");
                } else {
                    t->begin_min = mm;
                    snprintf(out->payload, sizeof out->payload, "begin=%s", val);
                }
            } else {
                if (t->begin_min >= 0 && mm <= t->begin_min) {
                    out->command = CMD_ERROR;
                    snprintf(out->payload, sizeof out->payload, "end deve seguire begin");
                } else {
                    t->end_min = mm;
                    snprintf(out->payload, sizeof out->payload, "end=%s", val);
                }
            }
        } else if (strcmp(label, "switch") == 0) {
            strncpy(t->sw_label, val, sizeof t->sw_label - 1);
            snprintf(out->payload, sizeof out->payload, "switch=%s", t->sw_label);
        } else {
            /* switch normale: propaga al figlio (modo scrittura) */
            if (t->child_id < 0) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "nessun figlio collegato");
            } else if (timer_send_child(t->child_id, in->payload) != 0) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "figlio %d non raggiungibile", t->child_id);
            } else {
                char st[64];
                if (timer_query_child(t->child_id, st, sizeof st) == 0)
                    strncpy(t->expected, st, sizeof t->expected - 1);
                snprintf(out->payload, sizeof out->payload, "propagato al figlio");
            }
        }
        break;
    }

    case CMD_LINK: {
        char label[32] = {0}, val[64] = {0};
        if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 2) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        } else if (strcmp(label, "child") == 0) {
            t->child_id = atoi(val);           /* il Timer controlla un solo figlio */
            t->expected[0] = '\0';
            snprintf(out->payload, sizeof out->payload, "child=%d", t->child_id);
        } else if (strcmp(label, "parent") == 0) {
            t->parent_id = atoi(val);
            snprintf(out->payload, sizeof out->payload, "parent=%d", t->parent_id);
        } else {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        }
        break;
    }

    default:
        out->command = CMD_ERROR;
        snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        break;
    }
}

/* Server del device. select() attende un comando ma, se c'e' una fascia oraria
 * impostata, con un limite pari al tempo che manca alla prossima azione (accensione
 * a begin, spegnimento a end): allo scadere il Timer aziona il figlio. */
void timer_run(int srv_fd, int id) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof(path), "/tmp/domotic_%d.sock", id);
    register_cleanup_handler(path);

    Timer t;
    timer_init(&t);
    srand((unsigned)getpid());
    fprintf(stderr, "[timer %d] avviato (pid=%d)\n\n", id, getpid());

    while (1) {
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
        if (action != -1) { tv.tv_sec = wait_sec; tv.tv_usec = 0; tvp = &tv; }

        int ready = select(srv_fd + 1, &rset, NULL, NULL, tvp);
        if (ready == -1) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }
        if (ready == 0) {
            timer_actuate(&t, action == 1);
            fprintf(stderr, "[timer %d] %s del figlio\n", id, action == 1 ? "accensione" : "spegnimento");
            continue;
        }

        int client = accept_connection(srv_fd);
        if (client == -1) continue;
        Message in;
        if (receive_message(client, &in) == 0) {
            sleep(1 + rand() % 3);
            Message out;
            handle_message(&t, &in, &out);
            send_message(client, &out);
        }
        close_connection(client);
    }
}