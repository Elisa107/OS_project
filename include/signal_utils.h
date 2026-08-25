#ifndef SIGNAL_UTILS_H
#define SIGNAL_UTILS_H

// configure the device to remove its own socket file from /tmp after a SIGTERM signal
// (no orphanes on the filesystem)
void register_cleanup_handler(const char *socket_path);

#endif