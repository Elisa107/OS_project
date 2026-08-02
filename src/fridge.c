/* fridge.c — Interaction device: Fridge (Domotics 2026) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>

#include "protocol.h"
#include "ipc_utils.h"

typedef enum { CLOSED = 0, OPEN = 1 } FridgeState;

typedef struct {
    FridgeState state;
    int    delay;         /* secondi dopo i quali il frigo aperto si auto-chiude */
    int    perc;          /* riempimento 0-100, solo da manual_interaction */
    double temp;
    double thermostat;    /* solo da manual_interaction */
    int    parent_id;
    time_t open_since;    /* istante di apertura (0 se chiuso) */
    time_t open_deadline; /* istante in cui scatta l'auto-chiusura */
} Fridge;

static void fridge_init(Fridge *f) {
    f->state = CLOSED; f->delay = 10;
    f->perc = 0; f->temp = 4.0; f->thermostat = 4.0; f->parent_id = -1;
    f->open_since = 0; f->open_deadline = 0;
}

/* Costruisce in 'out' la risposta al comando 'in'.
 * Payload di CMD_SWITCH nel formato "label:pos", es:
 *   open:1  close:1              (dal Controller o da manual)
 *   perc:50  thermostat:3  delay:5  (solo da manual_interaction) */
static void handle_message(Fridge *f, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender   = in->receiver;
    out->receiver = in->sender;
    out->command  = CMD_ACK;

    switch (in->command) {

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
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        } else if (strcmp(label, "open") == 0) {
            if (atoi(val) != 0) {                 /* apertura: (ri)avvia il timer */
                f->state = OPEN;
                f->open_since = time(NULL);
                f->open_deadline = f->open_since + f->delay;
            }
            snprintf(out->payload, sizeof out->payload, "state=%s", f->state==OPEN?"open":"closed");
        } else if (strcmp(label, "close") == 0) {
            if (atoi(val) != 0) {
                f->state = CLOSED;
                f->open_since = 0;
                f->open_deadline = 0;
            }
            snprintf(out->payload, sizeof out->payload, "state=%s", f->state==OPEN?"open":"closed");
        } else if (strcmp(label, "perc") == 0) {
            int v = atoi(val);
            if (v < 0 || v > 100) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
            } else {
                f->perc = v;
                snprintf(out->payload, sizeof out->payload, "perc=%d", f->perc);
            }
        } else if (strcmp(label, "thermostat") == 0) {
            f->thermostat = atof(val);
            snprintf(out->payload, sizeof out->payload, "thermostat=%.1f", f->thermostat);
        } else if (strcmp(label, "delay") == 0) {
            int d = atoi(val);
            if (d < 0) {
                out->command = CMD_ERROR;
                snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
            } else {
                f->delay = d;
                if (f->state == OPEN) f->open_deadline = f->open_since + f->delay;
                snprintf(out->payload, sizeof out->payload, "delay=%d", f->delay);
            }
        } else {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        }
        break;
    }

    case CMD_LINK: {                              /* imposta il genitore logico */
        int pid = -1;
        if (sscanf(in->payload, "%d", &pid) != 1) pid = in->sender;
        f->parent_id = pid;
        snprintf(out->payload, sizeof out->payload, "parent=%d", f->parent_id);
        break;
    }

    default:
        out->command = CMD_ERROR;
        snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        break;
    }
}

/* Server del device: una connessione per comando.
 * select() aspetta un comando ma, se il frigo e' aperto, con un tempo limite:
 * se non arriva nulla entro `delay` secondi, il frigo si auto-chiude. */
void fridge_run(int srv_fd, int id) {
    Fridge f;
    fridge_init(&f);
    srand((unsigned)getpid());
    fprintf(stderr, "[fridge %d] avviato (pid=%d)\n", id, getpid());

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
            tvp = &tv;                 /* aperto: attende al massimo `remaining` s */
        } else {
            tvp = NULL;                /* chiuso: attende un comando senza scadenza */
        }

        int ready = select(srv_fd + 1, &rset, NULL, NULL, tvp);

        if (ready == -1) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }

        if (ready == 0) {              /* scaduto il tempo -> auto-chiusura */
            f.state = CLOSED;
            f.open_since = 0;
            f.open_deadline = 0;
            fprintf(stderr, "[fridge %d] auto-chiusura (delay %d s scaduto)\n", id, f.delay);
            continue;
        }

        int client = accept_connection(srv_fd);
        if (client == -1) continue;

        Message in;
        if (receive_message(client, &in) == 0) {
            sleep(1 + rand() % 3);     /* ritardo di elaborazione simulato (1-3 s) */
            Message out;
            handle_message(&f, &in, &out);
            send_message(client, &out);
        }
        close_connection(client);
    }
}