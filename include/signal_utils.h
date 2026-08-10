#ifndef SIGNAL_UTILS_H
#define SIGNAL_UTILS_H

// registra il gestore di SIGTERM per il device corrente,
// passando il percorso del socket da rimuovere alla chiusura
void register_cleanup_handler(const char *socket_path);

#endif