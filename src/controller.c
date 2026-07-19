#include <sys/wait.h>
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

int controller_init(){
    

}

// it reads commands from stdin and executes them, until the user types "exit"
int controller_shell(){
    char line[256];

    while (1) {
        printf("> ");
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
                int id, pos;
                char label[32];
                sscanf(line, "switch %d %s %d", &id, label, &pos);
                int result = switch_device(id, label, pos);
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
            case SHELL_EXIT:
                return SUCCESS;
            default:
                printf("Command not recognized\n");
        }       
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

        // per ora, solo un placeholder: accetta una connessione e chiude
        // (qui dopo ci metterete la vera logica del device)
        printf("Device %d started, listening on %s\n", new_id, path);

        exit(0);  // il figlio termina quando la sua logica finisce (per ora subito)
        /*Quando il figlio termina (subito, per ora), diventa uno "zombie" finché il 
        padre non fa wait()/waitpid() su di lui. Per ora, con un placeholder che esce 
        subito, non è un problema bloccante — ma quando il device farà qualcosa di persistente 
        (restare in ascolto sul socket in un loop), il figlio non uscirà più subito, e non dovrai
        preoccuparti di questo nell'immediato.*/
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

int list_devices(){
    if(device_count == 0) {
        return NO_DEVICES;
    }
    for(int i = 0; i < device_count; i++) {
        if(devices[i].active) {
            printf("Device ID: %d, Type: %s, Role: %s\n", 
            devices[i].id, 
            type_to_string(devices[i].type), 
            is_control_device(devices[i].type) ? "Control" : "Interaction");
        }
    }
    return SUCCESS;
}

int switch_device(int device_id, char* label, int switch_pos){
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
    if (is_control_device(devices[index].type)) {
        return DEVICE_TYPE_MISMATCH;
    }
    int fd = connect_device(devices[index].socket_path);
    if (fd == -1) {
        return IPC_ERROR;
    }

    Message request;
    request.command = CMD_SWITCH;
    request.sender = 0;
    request.receiver = device_id;
    snprintf(request.payload, sizeof(request.payload), "%s:%d", label, switch_pos);

    send_message(fd, &request);

    Message response;
    receive_message(fd, &response);

    if (response.command == CMD_ACK) {
        printf("Switch updated: %s\n", response.payload);
    } else {
        printf("Errore nell'aggiornamento dello switch\n");
    }

    close_connection(fd);
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
    return SUCCESS;
}

int delete_device(int device_id) {
    if (device_id < 0 || device_id >= next_id || !devices[device_id].active) {
        return DEVICE_NOT_FOUND;
    }
    kill(devices[device_id].pid, SIGTERM);
    devices[device_id].active = 0;
    return SUCCESS;
}


