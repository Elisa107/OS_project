#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../include/ipc_utils.h"
#include "../include/common.h"

int create_server(int device_id, char *path) {
    snprintf(path, SOCKET_PATH_LEN, "/tmp/domotic_%d.sock", device_id);
    remove_socket(path);  // remove the file if it already exists, to avoid bind errors

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv == -1) { 
        perror("socket"); 
        return -1; 
    }

    struct sockaddr_un addr; // not contains IP+port but only a file's path (we're working locally)
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); 
        close_connection(srv); 
        return -1;
    }

    if (listen(srv, 1) == -1) {
        perror("listen"); 
        close_connection(srv); 
        return -1;
    }

    printf("Device %d is listening on %s\n", device_id, path);
    return srv;
}

int accept_connection(int server_fd) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd == -1) { 
        perror("accept"); 
        return -1; 
    }
    return client_fd;
}

int connect_device(char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) { 
        perror("socket"); 
        return -1; 
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect"); 
        close_connection(fd); 
        return -1;
    }

    return fd;
}

int send_message(int fd, Message *msg) {
    ssize_t n = write(fd, msg, sizeof(Message));
    if (n == -1) { 
        perror("write"); 
        return -1; 
    }
    return 0;
}

int receive_message(int fd, Message *msg) {
    size_t total = 0;
    size_t to_read = sizeof(Message);
    char *buf = (char *) msg;

    while (total < to_read) {
        ssize_t n = read(fd, buf + total, to_read - total);
        if (n == -1) {
            perror("read"); 
            return -1; 
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    return 0;
}

void close_connection(int fd) {
    close(fd);
}

void remove_socket(const char *path) {
    unlink(path);
}

/* prende id, in, out, e fa sempre la stessa cosa (controlla se è un override manuale 
riuscito, si connette al Controller, manda CMD_OVERRIDE).*/
void notify_controller_override(int id, const Message *in, const Message *out) {
    if (in->sender != -1) return;
    if (in->command != CMD_SWITCH) return;
    if (out->command != CMD_ACK) return;

    char ctrl_path[SOCKET_PATH_LEN];
    snprintf(ctrl_path, sizeof(ctrl_path), "%s", CONTROLLER_SOCKET_PATH);

    int fd = connect_device(ctrl_path);
    if (fd == -1) return;

    Message notify;
    memset(&notify, 0, sizeof notify);
    notify.command = CMD_OVERRIDE;
    notify.sender = id;
    notify.receiver = 0;
    snprintf(notify.payload, sizeof notify.payload, "%s", out->payload);

    send_message(fd, &notify);
    close_connection(fd);
}

