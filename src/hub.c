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

/* Propaga uno switch a un figlio. Ritorna -1 se non raggiungibile o erroreù. */
static int hub_propagate_switch(int child_id, const char *payload, int sender) {
    char path[SOCKET_PATH_LEN];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", child_id);
    int fd = connect_device(path);
    if (fd == -1) return -1;

    Message req;
    memset(&req, 0, sizeof req);
    req.command = CMD_SWITCH; req.sender = sender; req.receiver = child_id;
    strncpy(req.payload, payload, sizeof req.payload - 1);
    if (send_message(fd, &req) != 0) { close_connection(fd); return -1; }

    Message resp;
    if (receive_message(fd, &resp) != 0) { close_connection(fd); return -1; }
    close_connection(fd);
    return (resp.command == CMD_ACK) ? 0 : -1;
}

/* "Lingua" dello switch di un figlio: dipende dal tipo di device.
 * La deduciamo dallo stato che il figlio riporta (on/off -> power, open/closed -> open/close). */
typedef enum { VOCAB_UNKNOWN, VOCAB_POWER, VOCAB_OPENCLOSE } Vocab;

static Vocab child_vocab(int child_id) {
    char st[64];
    if (hub_query_child(child_id, st, sizeof st) != 0) return VOCAB_UNKNOWN;
    if (strcmp(st, "on") == 0 || strcmp(st, "off") == 0)     return VOCAB_POWER;
    if (strcmp(st, "open") == 0 || strcmp(st, "closed") == 0) return VOCAB_OPENCLOSE;
    return VOCAB_UNKNOWN;
}

/* Manda a un figlio uno switch on/off tradotto nella sua lingua.
 * value: 1 = attiva (on/open), 0 = disattiva (off/closed).
 * Ritorna -1 se il figlio non è raggiungibile o non ne capiamo la lingua. */
static int hub_switch_child(int child_id, int value, int sender) {
    char payload[64];
    switch (child_vocab(child_id)) {
    case VOCAB_POWER:
        snprintf(payload, sizeof payload, "power:%d", value);
        break;
    case VOCAB_OPENCLOSE:
        /* gli switch open/close sono momentanei: per aprire "open:1", per chiudere "close:1" */
        snprintf(payload, sizeof payload, value ? "open:1" : "close:1");
        break;
    default:
        return -1;
    }
    return hub_propagate_switch(child_id, payload, sender);
}

/* Normalizza lo stato di un figlio a un valore binario:
 * 1 = attivo (on/open), 0 = spento (off/closed), -1 = sconosciuto. */
static int state_to_bit(const char *state) {
    if (strcmp(state, "on") == 0 || strcmp(state, "open") == 0)     return 1;
    if (strcmp(state, "off") == 0 || strcmp(state, "closed") == 0)  return 0;
    return -1;
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
        int consistent = 1, first_bit = -1;
        /* Interroga TUTTI i figli anche se qualcuno non e' raggiungibile:
         * stesso principio del fix gia' applicato a CMD_SWITCH sotto - un
         * figlio caduto non deve impedire di leggere lo stato degli altri. */
        int failed_count = 0;
        char failed_list[64] = {0};
        for (int i = 0; i < h->num_children; i++) {
            if (hub_query_child(h->children[i], st, sizeof st) != 0) {
                char tmp[16];
                snprintf(tmp, sizeof tmp, failed_count ? ",%d" : "%d", h->children[i]);
                strncat(failed_list, tmp, sizeof failed_list - strlen(failed_list) - 1);
                failed_count++;
                continue;
            }
            int bit = state_to_bit(st);         /* on/open -> 1, off/closed -> 0 */
            if (first_bit == -1) {              /* primo figlio raggiungibile: stato di riferimento */
                first_bit = bit;
                strncpy(first, st, sizeof first - 1);
            } else if (bit != first_bit) {      /* confronto logico, non a stringa */
                consistent = 0;
            }
        }
        if (failed_count > 0) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload,
                     "figli non raggiungibili: %s (%d su %d)",
                     failed_list, failed_count, h->num_children);
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
        /* All'hub interessa solo il valore on/off; l'etichetta (es. "power") la ignoriamo
         * e la ricostruiamo per ogni figlio nella sua lingua. */
        const char *colon = strchr(in->payload, ':');
        int value = colon ? atoi(colon + 1) : -1;
        if (value != 0 && value != 1) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload, "INVALID_ARGUMENT");
            break;
        }
        /* Propaga a TUTTI i figli anche se qualcuno non e' raggiungibile: un
         * figlio caduto non deve impedire agli altri (ancora vivi) di
         * ricevere il comando — prima un "break" al primo fallito lasciava
         * tutti i figli successivi non aggiornati. */
        int failed_count = 0;
        char failed_list[64] = {0};
        for (int i = 0; i < h->num_children; i++) {
            if (hub_switch_child(h->children[i], value, in->sender) != 0) {
                char tmp[16];
                snprintf(tmp, sizeof tmp, failed_count ? ",%d" : "%d", h->children[i]);
                strncat(failed_list, tmp, sizeof failed_list - strlen(failed_list) - 1);
                failed_count++;
            }
        }
        if (failed_count > 0) {
            out->command = CMD_ERROR;
            snprintf(out->payload, sizeof out->payload,
                     "figli non raggiungibili: %s (propagato agli altri %d)",
                     failed_list, h->num_children - failed_count);
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
            notify_controller_override(id, &in, &out);
        }
        close_connection(client);
    }
}