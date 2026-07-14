#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "protocol.h"

// Create a socket server which is associated with a path
int create_server(int device_id, char *path);

// Wait for a client to connect to the server socket
int accept_connection(int server_fd);

// It connects to a server socket at the specified path
int connect_device(char *path);

// Sends a message to the specified file descriptor
int send_message(int fd, Message *msg);

// Receives a message
int receive_message(int fd, Message *msg);

// Close a connection
void close_connection(int fd);

// Remove the socket file
void remove_socket(const char *path);

#endif