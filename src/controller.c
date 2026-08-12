#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/common.h"
#include "../include/errors.h"
#include "../include/protocol.h"
#include "../include/ipc_utils.h"
#include "../include/device.h"
#include "../include/controller.h"

/* AGGIUNTA (Evelin): include per bulb_run()/window_run(), usati in add_device() */
#include "../include/bulb.h"
#include "../include/window.h"
#include "../include/hub.h"
#include "../include/timer.h"
#include "../include/fridge.h"

typedef enum {
    SHELL_LIST,
    SHELL_ADD,
    SHELL_DEL,
    SHELL_LINK,
    SHELL_SWITCH,
    SHELL_INFO,
    SHELL_EXIT,
    SHELL_UNKNOWN
} ShellCommand;

ShellCommand parse_shell_command(char *cmd) {
    if (strcmp(cmd, "list") == 0) return SHELL_LIST;
    if (strcmp(cmd, "add") == 0) return SHELL_ADD;
    if (strcmp(cmd, "del") == 0) return SHELL_DEL;
    if (strcmp(cmd, "link") == 0) return SHELL_LINK;
    if (strcmp(cmd, "switch") == 0) return SHELL_SWITCH;
    if (strcmp(cmd, "info") == 0) return SHELL_INFO;
    if (strcmp(cmd, "exit") == 0) return SHELL_EXIT;
    return SHELL_UNKNOWN;
}

int next_id = 0; // keep track of the next device ID to assign
Device devices[MAX_DEVICES];
int device_count = 0;

static int controller_srv_fd = -1;

static int open_controller_socket(void) {
    unlink(CONTROLLER_SOCKET_PATH); // rimuove il file residuo di un run precedente

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv == -1) {
        perror("socket (controller)");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROLLER_SOCKET_PATH);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind (controller)");
        close(srv);
        return -1;
    }

    if (listen(srv, 10) == -1) {
        perror("listen (controller)");
        close(srv);
        return -1;
    }

    return srv;
}

static void handle_incoming_notification(void) {
    int client = accept_connection(controller_srv_fd);
    if (client == -1) return;

    Message in;
    if (receive_message(client, &in) == 0) {
        if (in.command == CMD_OVERRIDE) {
            printf("\n[NOTIFICA] Device %d: override manuale -> %s\n", in.sender, in.payload);
        }
    }
    close_connection(client);
}

/* Handler di SIGCHLD: quando un device figlio muore (crash o kill), il kernel
 * avvisa il Controller. Qui "raccogliamo" i processi morti con waitpid non
 * bloccante (WNOHANG) e segniamo i device corrispondenti come non piu' attivi,
 * rimuovendone il socket rimasto in /tmp. */
static void handle_sigchld(int sig) {
    (void)sig;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        for (int i = 0; i < next_id; i++) {
            if (devices[i].pid == pid && devices[i].active) {
                devices[i].active = 0;
                remove_socket(devices[i].socket_path);
                break;
            }
        }
    }
}

int controller_init(){
    /* Installa la rilevazione dei crash. SA_RESTART fa riprendere le chiamate
     * bloccanti (la lettura dei comandi) invece di interromperle quando muore
     * un device; SA_NOCLDSTOP evita di ricevere SIGCHLD quando un figlio si
     * limita a sospendersi. */
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        return IPC_ERROR;
    }
    return SUCCESS;
}

// it reads commands from stdin and executes them, until the user types "exit"
int controller_shell(){
    char line[256];

    controller_init();   /* installa la rilevazione crash (SIGCHLD) */

    controller_srv_fd = open_controller_socket();
    if (controller_srv_fd == -1) {
        fprintf(stderr, "Impossibile aprire il socket del Controller: le notifiche spontanee non funzioneranno\n");
    }

    printf("> ");
    fflush(stdout);

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        int maxfd = STDIN_FILENO;
        if (controller_srv_fd != -1) {
            FD_SET(controller_srv_fd, &readfds);
            if (controller_srv_fd > maxfd) maxfd = controller_srv_fd;
        }

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready == -1) {
            if (errno == EINTR) continue; /* interrotta da SIGCHLD, si ritenta */
            perror("select");
            break;
        }

        if (controller_srv_fd != -1 && FD_ISSET(controller_srv_fd, &readfds)) {
            handle_incoming_notification();
        }

        if (!FD_ISSET(STDIN_FILENO, &readfds)) {
            continue; /* era pronta solo la notifica: torna ad aspettare, stdin invariato */
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        line[strcspn(line, "\n")] = '\0';
        char command[32];
        sscanf(line, "%s", command);

        switch (parse_shell_command(command)) {
            case SHELL_LIST: {
                int result = list_devices();
                if (result != SUCCESS) {
                    printf("Error: %s\n", error_to_string(result));
                }
                break;
            }
            case SHELL_ADD: {
                char type_str[32];
                sscanf(line, "add %s", type_str);
                DeviceType type = parse_type(type_str);
                if (type == -1) {
                    printf("Invalid device type\n");
                    break;
                }
                int new_id = add_device(type, -1);
                printf("Device created with ID: %d\n", new_id);
                break;
            }
            case SHELL_DEL: {
                int id;
                sscanf(line, "del %d", &id);
                int result = delete_device(id);
                if (result != SUCCESS) {
                    printf("Error: %s\n", error_to_string(result));
                } else {
                    printf("Device %d deleted\n", id);
                }
                break;
            }
            case SHELL_LINK: {
                int id1, id2;
                sscanf(line, "link %d to %d", &id1, &id2);
                int result = link_devices(id1, id2);
                if (result != SUCCESS) {
                    printf("Error: %s\n", error_to_string(result));
                } else {
                    printf("Device %d linked to %d\n", id1, id2);
                }
                break;
            }
            case SHELL_SWITCH: {
                int id;
                char label[32], value[64];
                sscanf(line, "switch %d %s %s", &id, label, value);
                int result = switch_device(id, label, value);
                if (result != SUCCESS) {
                    printf("Error: %s\n", error_to_string(result));
                } else {
                    printf("Switch updated successfully\n");
                }
                break;
            }
            case SHELL_INFO: {
                int id;
                char output[512];
                sscanf(line, "info %d", &id);
                int result = info(id, output);
                if (result != SUCCESS) {
                    printf("Error: %s\n", error_to_string(result));
                } else {
                    printf("%s", output);
                }
                break;
            }
            case SHELL_EXIT: {
                int i;
                for (i = 0; i < device_count; i++) {
                    if (devices[i].active) {
                        kill(devices[i].pid, SIGTERM);
                        waitpid(devices[i].pid, NULL, 0);
                    }
                }
                return SUCCESS;
            }
            default:
                printf("Command not recognized\n");
        }
        printf("> ");
        fflush(stdout);    
    }
    return SUCCESS;
}

int add_device(DeviceType type, int parent_id) {
    int new_id = next_id++;
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return IPC_ERROR;
    }

    if (pid == 0){
        // child (this process will become the new device)
        char path[SOCKET_PATH_LEN];
        int srv_fd = create_server(new_id, path);
        if (srv_fd == -1) {
            exit(1);
        }

        printf("Device %d started, listening on %s\n", new_id, path);

        /* AGGIUNTA (Evelin): dispatch di Bulb/Window verso bulb_run()/window_run(),
         * che non ritornano mai (restano in ascolto finché non ricevono SIGTERM) */
        if (type == BULB) {
            bulb_run(srv_fd, new_id);
        } else if (type == WINDOW) {
            window_run(srv_fd, new_id);
        } else if (type == HUB) {
            hub_run(srv_fd, new_id);
        } else if (type == TIMER) {
            timer_run(srv_fd, new_id);
        } else if (type == FRIDGE) {
            fridge_run(srv_fd, new_id);
        }

        /* MODIFICA (Evelin): commento originale spostato qui sotto (prima era
         * subito dopo "child (this process...)") e riformulato da "per ora,
         * solo un placeholder: accetta una connessione e chiude" a quanto
         * segue, perché ora vale solo per i tipi non ancora collegati sopra. */
        // placeholder originale per i tipi non ancora collegati
        // (qui dopo ci metterete la vera logica del device)
        exit(0);  // il figlio termina quando la sua logica finisce (per ora subito)
    }

    // Controller
    devices[new_id].id = new_id;
    devices[new_id].pid = pid;
    devices[new_id].type = type;
    devices[new_id].parent_id = parent_id;
    devices[new_id].active = 1;
    device_count++;

    // it saves also the socket_path which will be used by the child 
    snprintf(devices[new_id].socket_path, SOCKET_PATH_LEN, "/tmp/domotic_%d.sock", new_id);

    return new_id;
}

int info(int device_id, char *output){
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == device_id) {

            if (!devices[i].active) {
                return DEVICE_NOT_FOUND;
            }

            int fd = connect_device(devices[i].socket_path);
            if (fd == -1) {
                return IPC_ERROR;
            }

            Message request;
            request.command = CMD_INFO;
            request.sender = 0;
            request.receiver = device_id;
            send_message(fd, &request);

            Message response;
            receive_message(fd, &response);
            close_connection(fd);

            snprintf(output, 512,
                "Device ID: %d\nType: %s\nRole: %s\nState/Switches: %s\n",
                devices[i].id,
                type_to_string(devices[i].type),
                is_control_device(devices[i].type) ? "Control" : "Interaction",
                response.payload);

            return SUCCESS;
        }
    }
    return DEVICE_NOT_FOUND;
}

int list_devices() {
    if (device_count == 0) {
        return NO_DEVICES;
    }
    for (int i = 0; i < device_count; i++) {
        if (!devices[i].active) continue;

        char state_info[256] = "N/A";
        int fd = connect_device(devices[i].socket_path);
        if (fd != -1) {
            Message req, resp;
            req.command = CMD_INFO;
            req.sender = 0;
            req.receiver = devices[i].id;
            send_message(fd, &req);
            receive_message(fd, &resp);
            snprintf(state_info, sizeof(state_info), "%s", resp.payload);
            close_connection(fd);
        }

        printf("ID: %d, Type: %s, Role: %s, State: %s\n",
               devices[i].id, type_to_string(devices[i].type),
               is_control_device(devices[i].type) ? "Control" : "Interaction",
               state_info);
    }
    return SUCCESS;
}

int switch_device(int device_id, char* label, char* value){
    int index = -1;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].id == device_id && devices[i].active) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        return DEVICE_NOT_FOUND;
    }
    int fd = connect_device(devices[index].socket_path);
    if (fd == -1) {
        return IPC_ERROR;
    }

    Message request;
    request.command = CMD_SWITCH;
    request.sender = 0;
    request.receiver = device_id;
    snprintf(request.payload, sizeof(request.payload), "%s:%s", label, value);

    send_message(fd, &request);

    Message response;
    receive_message(fd, &response);
    close_connection(fd);

    return (response.command == CMD_ACK) ? SUCCESS : SWITCH_REJECTED;
}

// Check if linking device_id to parent_id would create a cycle
int creates_cycle(int device_id, int parent_id) {
    int current = parent_id;
    while (current != -1) {
        if (current == device_id) {
            return 1;
        }
        current = devices[current].parent_id;
    }
    return 0;  // no cycles
}

int link_devices(int device_id, int parent_id){
    if (device_id < 0 || device_id >= next_id || !devices[device_id].active) {
        return DEVICE_NOT_FOUND;
    }
    if (parent_id < 0 || parent_id >= next_id || !devices[parent_id].active) {
        return DEVICE_NOT_FOUND;
    }
    if (device_id == parent_id) {
        return LINK_FAILED;
    }
    if (!is_control_device(devices[parent_id].type)) {
        return LINK_FAILED;
    }
    if (creates_cycle(device_id, parent_id)) {
        return LINK_FAILED;
    }

    devices[device_id].parent_id = parent_id;

    // avvisa il device genitore (se è un HUB o TIMER) del nuovo figlio
    int fd = connect_device(devices[parent_id].socket_path);
    if (fd != -1) {
        Message msg;
        msg.command = CMD_LINK;
        msg.sender = 0;
        msg.receiver = parent_id;
        snprintf(msg.payload, sizeof(msg.payload), "child:%d", device_id);
        send_message(fd, &msg);

        Message resp;
        receive_message(fd, &resp);
        close_connection(fd);
    }

    int fd_child = connect_device(devices[device_id].socket_path);
    if (fd_child != -1) {
        Message msg_child;
        msg_child.command = CMD_LINK;
        msg_child.sender = 0;
        msg_child.receiver = device_id;
        snprintf(msg_child.payload, sizeof(msg_child.payload), "%d", parent_id);
        send_message(fd_child, &msg_child);

        Message resp_child;
        receive_message(fd_child, &resp_child);
        close_connection(fd_child);
    }

    return SUCCESS;
}

int delete_device(int device_id) {
    int i;
    if (device_id < 0 || device_id >= next_id || !devices[device_id].active) {
        return DEVICE_NOT_FOUND;
    }
    // prima cancella ricorsivamente tutti i figli logici (se ce ne sono)
    for (i = 0; i < device_count; i++) {
        if (devices[i].active && devices[i].parent_id == device_id) {
            delete_device(devices[i].id);   // ricorsione: cancella anche i "nipoti"
        }
    }
    // poi termina il device stesso
    kill(devices[device_id].pid, SIGTERM);
    devices[device_id].active = 0;
    devices[device_id].parent_id = -1;

    return SUCCESS;
}