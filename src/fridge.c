/* fridge.c — Interaction device: Fridge (Domotics 2026) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"
#include "ipc_utils.h"

typedef enum { CLOSED = 0, OPEN = 1 } FridgeState;

typedef struct {
    FridgeState state;
    int    time;
    int    delay;
    int    perc;        /* solo manuale */
    double temp;
    double thermostat;  /* solo manuale */
    int    parent_id;
} Fridge;

static void fridge_init(Fridge *f) {
    f->state = CLOSED; f->time = 0; f->delay = 10;
    f->perc = 0; f->temp = 4.0; f->thermostat = 4.0; f->parent_id = -1;
}

/* Costruisce la risposta out al comando in. */
static void handle_message(Fridge *f, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender   = in->receiver;   /* rispondo scambiando sender/receiver */
    out->receiver = in->sender;
    out->command  = CMD_ACK;

    switch (in->command) {

    case CMD_INFO:
        snprintf(out->payload, sizeof out->payload,
            "state=%s time=%d delay=%d perc=%d temp=%.1f thermostat=%.1f",
            f->state == OPEN ? "open" : "closed",
            f->time, f->delay, f->perc, f->temp, f->thermostat);
        break;

    /* payload = "label:pos"  es. "open:1", "close:1",
     * "perc:50", "thermostat:3" (questi due solo da manual_interaction). */
    case CMD_SWITCH: {
        char label[32] = {0}, val[64] = {0};
        if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 1) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        } else if (strcmp(label, "open") == 0) {
            if (atoi(val) != 0) f->state = OPEN;
            snprintf(out->payload, sizeof out->payload, "state=%s", f->state==OPEN?"open":"closed");
        } else if (strcmp(label, "close") == 0) {
            if (atoi(val) != 0) f->state = CLOSED;
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
        } else {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        }
        break;
    }

    /* Un interaction device puo' solo ricevere un nuovo genitore logico. */
    case CMD_LINK: {
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

/* Corpo del device: server che accetta una connessione per comando
 * (come si aspetta il Controller: connect -> send -> receive -> close).
 * srv_fd e' il socket in ascolto restituito da create_server(). */
void fridge_run(int srv_fd, int id) {
    Fridge f;
    fridge_init(&f);
    srand((unsigned)getpid());
    fprintf(stderr, "[fridge %d] avviato (pid=%d)\n", id, getpid());

    while (1) {
        int client = accept_connection(srv_fd);
        if (client == -1) continue;

        Message in;
        if (receive_message(client, &in) == 0) {
            sleep(1 + rand() % 3);   /* 1-3s: ritardo di elaborazione (traccia 2.2.6) */
            Message out;
            handle_message(&f, &in, &out);
            send_message(client, &out);
        }
        close_connection(client);
    }
}

/* Test standalone: simula il Controller (una connessione per comando).
 * gcc -DFRIDGE_STANDALONE -Iinclude -o /tmp/ft src/fridge.c src/ipc_utils.c */
#ifdef FRIDGE_STANDALONE
#include <sys/wait.h>
#include <signal.h>
#include "common.h"

static void ask(const char *label, Command c, const char *payload, int id, char *path) {
    int fd = connect_device(path);
    if (fd < 0) { printf("%-16s connect fallita\n", label); return; }
    Message m; memset(&m, 0, sizeof m);
    m.command = c; m.sender = 0; m.receiver = id;
    if (payload) strncpy(m.payload, payload, sizeof m.payload - 1);
    send_message(fd, &m);
    Message r; receive_message(fd, &r);
    close_connection(fd);
    printf("%-16s <- %s: %s\n", label, r.command == CMD_ACK ? "ACK" : "ERR", r.payload);
}

int main(void) {
    int id = 3;
    char path[SOCKET_PATH_LEN];

    pid_t pid = fork();
    if (pid == 0) {                       /* figlio: il device */
        int srv = create_server(id, path);
        if (srv < 0) _exit(1);
        fridge_run(srv, id);
        _exit(0);
    }

    snprintf(path, SOCKET_PATH_LEN, "/tmp/domotic_%d.sock", id);
    sleep(1);                             /* attende che il server sia pronto */
    ask("INFO",        CMD_INFO,   "",        id, path);
    ask("open:1",      CMD_SWITCH, "open:1",  id, path);
    ask("perc:50",     CMD_SWITCH, "perc:50", id, path);
    ask("INFO",        CMD_INFO,   "",        id, path);

    kill(pid, SIGTERM);                   /* come fa delete_device del Controller */
    wait(NULL);
    remove_socket(path);
    return 0;
}
#endif