#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "signal_utils.h"
#include "ipc_utils.h"
#include "common.h"

// signal handler SIGTERM in devices to clean the socket before the exit
static char cleanup_path[SOCKET_PATH_LEN];

static void handle_sigterm(int sig) {
    remove_socket(cleanup_path);
    exit(0);
}

void register_cleanup_handler(const char *socket_path) {
    strncpy(cleanup_path, socket_path, SOCKET_PATH_LEN - 1);
    signal(SIGTERM, handle_sigterm);
}