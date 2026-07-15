#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "protocol.h"

int create_server(int device_id, char *path);
int accept_connection(int server_fd);
int connect_device(char *path);
int send_message(int fd, Message *msg);
int receive_message(int fd, Message *msg);
void close_connection(int fd);
void remove_socket(const char *path);

#endif