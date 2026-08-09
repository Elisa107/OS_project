#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#include "protocol.h"
#include "ipc_utils.h"
#include "bulb.h"
#include "window.h"

#define BULB_ID   0
#define WINDOW_ID 1

static void req(int id, int sender, Command cmd, const char *payload, Message *out) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", id);

    int fd;
    for (int i = 0; i < 100 && (fd = connect_device(path)) == -1; i++) usleep(20000);
    if (fd == -1) { fprintf(stderr, "connessione fallita verso device %d\n", id); exit(1); }

    Message m; memset(&m, 0, sizeof m);
    m.command = cmd; m.sender = sender; m.receiver = id;
    if (payload) strncpy(m.payload, payload, sizeof m.payload - 1);

    send_message(fd, &m);
    receive_message(fd, out);
    close_connection(fd);
}

// SCENARIO 1 — comportamento base di Bulb e Window

static void scenario_base(void) {
    Message r;

    printf("\nSCENARIO 1: comportamento base\n");

    printf("\n--- BULB (id=%d) ---\n", BULB_ID);
    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info iniziale                 -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  switch power:1 (accensione)   -> %s\n", r.payload);

    sleep(2);
    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info dopo ~2s acceso          -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "power:0", &r);
    printf("  switch power:0 (spegnimento)  -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info dopo spegnimento         -> %s   (time e' rimasto cumulato)\n", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "power:5", &r);
    printf("  switch power:5 (valore non 0/1, atteso errore) -> cmd=%s payload=%s\n",
        r.command == CMD_ERROR ? "CMD_ERROR" : "???", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "pluto:1", &r);
    printf("  switch pluto:1 (label inesistente, atteso errore) -> cmd=%s payload=%s\n",
        r.command == CMD_ERROR ? "CMD_ERROR" : "???", r.payload);

    printf("\n--- WINDOW (id=%d) ---\n", WINDOW_ID);
    req(WINDOW_ID, 0, CMD_INFO, NULL, &r);
    printf("  info iniziale                 -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_SWITCH, "open:1", &r);
    printf("  switch open:1 (apertura)      -> %s\n", r.payload);

    sleep(2);
    req(WINDOW_ID, 0, CMD_INFO, NULL, &r);
    printf("  info dopo ~2s aperta          -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_SWITCH, "open:0", &r);
    printf("  switch open:0 (ritorno impulsivo a riposo, deve essere IGNORATO) -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_SWITCH, "close:1", &r);
    printf("  switch close:1 (chiusura)     -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_INFO, NULL, &r);
    printf("  info dopo chiusura            -> %s   (time e' rimasto cumulato)\n", r.payload);
}


//SCENARIO 2 — edge case 2.2.8: override manuale simultaneo a un

static void scenario_concurrent_override(void) {
    printf("\n=========== SCENARIO 2: override manuale simultaneo (edge case 2.2.8) ===========\n");
    printf("Riporto il Bulb a un stato noto (off) prima del test...\n");
    Message r;
    req(BULB_ID, 0, CMD_SWITCH, "power:0", &r);

    printf("Lancio DUE processi che inviano un comando allo STESSO Bulb (id=%d)\n", BULB_ID);
    printf("nello stesso istante: uno con sender=0 (Controller), uno con sender=-1\n");
    printf("(manual_interaction). Ognuno dei due device impiega 1-3s a rispondere.\n\n");

    fflush(stdout);   /* evita di duplicare l'output bufferizzato nei due figli */
    pid_t p1 = fork();
    if (p1 == 0) {
        Message out;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        req(BULB_ID, 0 /* Controller */, CMD_SWITCH, "power:1", &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("  [Controller ] switch power:1 -> %-12s (%.2fs)\n", out.payload, elapsed);
        exit(0);
    }

    fflush(stdout);
    pid_t p2 = fork();
    if (p2 == 0) {
        Message out;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        req(BULB_ID, -1 /* manual_interaction */, CMD_SWITCH, "power:0", &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("  [Manuale    ] switch power:0 -> %-12s (%.2fs)\n", out.payload, elapsed);
        exit(0);
    }

    int status;
    waitpid(p1, &status, 0);
    waitpid(p2, &status, 0);

    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("\n  Stato finale del Bulb dopo i due comandi concorrenti -> %s\n", r.payload);
    printf("\n  Osservazione: uno dei due comandi e' partito 'in coda' (tempo di risposta\n");
    printf("  piu' alto) perche' bulb_run() gestisce una connessione alla volta con\n");
    printf("  accept() bloccante: il sistema operativo mette in attesa la seconda\n");
    printf("  connessione finche' la prima non e' chiusa. Il device non riceve mai i\n");
    printf("  due comandi 'mescolati' e non finisce mai in uno stato indefinito: lo\n");
    printf("  stato finale corrisponde sempre a UNO dei due comandi per intero, mai\n");
    printf("  a una via di mezzo incoerente.\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    char path0[128], path1[128];
    int srv0 = create_server(BULB_ID, path0);
    int srv1 = create_server(WINDOW_ID, path1);

    fflush(stdout);
    pid_t bulb_pid = fork();
    if (bulb_pid == 0) { bulb_run(srv0, BULB_ID); exit(0); }

    fflush(stdout);
    pid_t window_pid = fork();
    if (window_pid == 0) { window_run(srv1, WINDOW_ID); exit(0); }

    scenario_base();
    scenario_concurrent_override();

    printf("\nfine dimostrazione\n");

    kill(bulb_pid, SIGTERM);
    kill(window_pid, SIGTERM);
    waitpid(bulb_pid, NULL, 0);
    waitpid(window_pid, NULL, 0);
    remove_socket(path0);
    remove_socket(path1);

    return 0;
}