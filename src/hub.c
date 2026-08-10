#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "common.h"
#include "signal_utils.h"

#define MAX_CHILDREN 32

typedef struct {
    int children[MAX_CHILDREN];  /* id dei device figli (l'Hub non salva il loro stato) */
    int num_children;
    int parent_id;
} Hub;

static void hub_init(Hub *h) {
    h->num_children = 0;
    h->parent_id = -1;
}

static void hub_add_child(Hub *h, int child_id) {
    for (int i = 0; i < h->num_children; i++)
        if (h->children[i] == child_id) return;
    if (h->num_children < MAX_CHILDREN)
        h->children[h->num_children++] = child_id;
}

/* Estrae il valore di "state=..." (fino al primo spazio) dal payload di un figlio. */
static void extract_state(const char *payload, char *out, size_t n) {
    const char *p = strstr(payload, "state=");
    if (!p) { snprintf(out, n, "?"); return; }
    p += 6;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

/* Interroga un figlio (INFO) e ne ricava lo stato. Ritorna -1 se non raggiungibile. */
static int hub_query_child(int child_id, char *state_out, size_t n) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1) return -1;

    Message req;
    memset(&req, 0, sizeof req);
    req.command = CMD_INFO; req.sender = -1; req.receiver = child_id;
    if (send_message(fd, &req) != 0) { close_connection(fd); return -1; }

    Message resp;
    if (receive_message(fd, &resp) != 0) { close_connection(fd); return -1; }
    close_connection(fd);

    extract_state(resp.payload, state_out, n);
    return 0;
}

/* Propaga uno switch a un figlio. Ritorna -1 se non raggiungibile o errore. */
static int hub_propagate_switch(int child_id, const char *payload) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1) return -1;

    Message req;
    memset(&req, 0, sizeof req);
    req.command = CMD_SWITCH; req.sender = -1; req.receiver = child_id;
    strncpy(req.payload, payload, sizeof req.payload - 1);
    if (send_message(fd, &req) != 0) { close_connection(fd); return -1; }

    Message resp;
    if (receive_message(fd, &resp) != 0) { close_connection(fd); return -1; }
    close_connection(fd);
    return (resp.command == CMD_ACK) ? 0 : -1;
}

/* Costruisce in 'out' la risposta al comando 'in'.
 * Payload CMD_LINK: "child:<id>" (nuovo figlio) o "parent:<id>" (nuovo genitore). */
static void handle_message(Hub *h, const Message *in, Message *out) {
    memset(out, 0, sizeof *out);
    out->sender   = in->receiver;
    out->receiver = in->sender;
    out->command  = CMD_ACK;

    switch (in->command) {

    /* Lettura: interroga TUTTI i figli dal vivo; se sono in disaccordo -> manual override. */
    case CMD_INFO: {
        if (h->num_children == 0) {
            snprintf(out->payload, sizeof out->payload, "state=empty (0 figli)");
            break;
        }
        char first[64] = {0}, st[64];
        int consistent = 1, crashed = -1, got = 0;
        for (int i = 0; i < h->num_children; i++) {
            if (hub_query_child(h->children[i], st, sizeof st) != 0) {
                crashed = h->children[i];       /* figlio non raggiungibile */
                break;
            }
            if (!got) { strncpy(first, st, sizeof first - 1); got = 1; }
            else if (strcmp(st, first) != 0) consistent = 0;
        }
        if (crashed != -1) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "figlio %d non raggiungibile", crashed);
        } else if (consistent) {
            snprintf(out->payload, sizeof out->payload, "state=%s (%d figli)", first, h->num_children);
        } else {
            snprintf(out->payload, sizeof out->payload, "state=manual override");
        }
        break;
    }

    /* Scrittura: propaga lo switch a TUTTI i figli (ristabilisce la coerenza). */
    case CMD_SWITCH: {
        if (h->num_children == 0) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "nessun figlio a cui propagare");
            break;
        }
        int crashed = -1;
        for (int i = 0; i < h->num_children; i++) {
            if (hub_propagate_switch(h->children[i], in->payload) != 0) {
                crashed = h->children[i];
                break;
            }
        }
        if (crashed != -1) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "figlio %d non raggiungibile", crashed);
        } else {
            snprintf(out->payload, sizeof out->payload, "propagato a %d figli", h->num_children);
        }
        break;
    }

    case CMD_LINK: {
        char label[32] = {0}, val[64] = {0};
        if (sscanf(in->payload, "%31[^:]:%63s", label, val) < 2) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_COMMAND");
        } else if (strcmp(label, "child") == 0) {
            hub_add_child(h, atoi(val));
            snprintf(out->payload, sizeof out->payload, "child %d aggiunto (tot %d)",
                     atoi(val), h->num_children);
        } else if (strcmp(label, "parent") == 0) {
            h->parent_id = atoi(val);
            snprintf(out->payload, sizeof out->payload, "parent=%d", h->parent_id);
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

/* Server del device: una connessione per comando. */
void hub_run(int srv_fd, int id) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof(path), "/tmp/domotic_%d.sock", id);
    register_cleanup_handler(path);

    Hub h;
    hub_init(&h);
    srand((unsigned)getpid());
    fprintf(stderr, "[hub %d] avviato (pid=%d)\n\n", id, getpid());

    while (1) {
        int client = accept_connection(srv_fd);
        if (client == -1) continue;

        Message in;
        if (receive_message(client, &in) == 0) {
            sleep(1 + rand() % 3);
            Message out;
            handle_message(&h, &in, &out);
            send_message(client, &out);
        }
        close_connection(client);
    }
}