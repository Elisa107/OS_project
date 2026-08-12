/* demo_all_devices.c — dimostrazione end-to-end di TUTTI i tipi di device
 * (Bulb, Window, Fridge come Interaction Device; Hub e Timer come Control
 * Device), senza passare dalla shell del Controller: ogni device viene
 * avviato con fork()+*_run() e interrogato via IPC direttamente, come
 * farebbe il Controller stesso.
 *
 * Serve come scenario "run" richiesto dalla traccia (vedi Makefile, target
 * "demo") e come prova riproducibile degli edge case discussi nel report.
 */

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
#include "fridge.h"
#include "hub.h"
#include "timer.h"

#define BULB_ID    0
#define WINDOW_ID  1
#define FRIDGE_ID  2
#define HUB_ID     3
#define TIMER_ID   4
#define BULB2_ID   5   /* secondo bulb, usato dal Timer (per non condividere il figlio con l'Hub) */

#define NUM_DEVICES 6

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

    printf("\n=========== SCENARIO 1: comportamento base Bulb/Window ===========\n");

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


// SCENARIO 2 — comportamento del Fridge (delay, perc, thermostat, auto-chiusura)

static void scenario_fridge(void) {
    Message r;

    printf("\n=========== SCENARIO 2: comportamento Fridge ===========\n");

    req(FRIDGE_ID, 0, CMD_INFO, NULL, &r);
    printf("  info iniziale                       -> %s\n", r.payload);

    req(FRIDGE_ID, -1, CMD_SWITCH, "perc:40", &r);
    printf("  set perc:40 (solo da manuale)       -> %s\n", r.payload);

    req(FRIDGE_ID, -1, CMD_SWITCH, "thermostat:3", &r);
    printf("  set thermostat:3 (solo da manuale)  -> %s\n", r.payload);

    req(FRIDGE_ID, -1, CMD_SWITCH, "delay:2", &r);
    printf("  set delay:2 (auto-chiusura a 2s)    -> %s\n", r.payload);

    req(FRIDGE_ID, 0, CMD_SWITCH, "open:1", &r);
    printf("  switch open:1 (apertura)            -> %s\n", r.payload);

    req(FRIDGE_ID, 0, CMD_INFO, NULL, &r);
    printf("  info subito dopo apertura           -> %s\n", r.payload);

    printf("  ... aspetto 3s (piu' del delay=2s impostato) senza mandare comandi ...\n");
    sleep(3);

    req(FRIDGE_ID, 0, CMD_INFO, NULL, &r);
    printf("  info dopo il timeout                -> %s   (atteso: state=closed, si e' auto-chiuso)\n", r.payload);
}


// SCENARIO 3 — Hub: propagazione a piu' figli e rilevazione di un manual override

static void scenario_hub(void) {
    Message r;

    printf("\n=========== SCENARIO 3: Hub (propagazione + rilevazione override) ===========\n");

    req(HUB_ID, 0, CMD_LINK, "child:0", &r);   /* Bulb   come figlio dell'Hub */
    printf("  link bulb(%d) all'hub    -> %s\n", BULB_ID, r.payload);

    req(HUB_ID, 0, CMD_LINK, "child:1", &r);   /* Window come figlio dell'Hub */
    printf("  link window(%d) all'hub  -> %s\n", WINDOW_ID, r.payload);

    req(HUB_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  switch hub power:1 (propaga a entrambi i figli) -> %s\n", r.payload);

    req(HUB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info hub (i figli sono coerenti)    -> %s   (atteso: on, non manual override)\n", r.payload);

    printf("  ... ora simulo un intervento manuale SOLO sul bulb, bypassando l'hub ...\n");
    req(BULB_ID, -1, CMD_SWITCH, "power:0", &r);
    printf("  manual_interaction su bulb power:0  -> %s\n", r.payload);

    req(HUB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info hub (figli ora disallineati)   -> %s   (atteso: state=manual override)\n", r.payload);

    req(HUB_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  nuovo switch hub power:1 (deve ristabilire coerenza, azzerando l'override) -> %s\n", r.payload);

    req(HUB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info hub dopo il nuovo comando      -> %s   (atteso: torna coerente)\n", r.payload);
}


// SCENARIO 4 — Timer: propagazione al figlio collegato + validazione begin/end

static void scenario_timer(void) {
    Message r;

    printf("\n=========== SCENARIO 4: Timer (figlio collegato + validazione orari) ===========\n");

    req(TIMER_ID, 0, CMD_LINK, "child:5", &r);   /* Bulb2 come figlio del Timer */
    printf("  link bulb2(%d) al timer            -> %s\n", BULB2_ID, r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "begin:08:00", &r);
    printf("  set begin:08:00                    -> %s\n", r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "end:07:00", &r);
    printf("  set end:07:00 (PRIMA di begin, atteso errore) -> cmd=%s payload=%s\n",
        r.command == CMD_ERROR ? "CMD_ERROR" : "???", r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "end:20:00", &r);
    printf("  set end:20:00 (valido)             -> %s\n", r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  switch power:1 (propaga al figlio, come farebbe l'orario) -> %s\n", r.payload);

    req(TIMER_ID, 0, CMD_INFO, NULL, &r);
    printf("  info timer                         -> %s\n", r.payload);

    req(BULB2_ID, 0, CMD_INFO, NULL, &r);
    printf("  info bulb2 diretto (deve confermare on) -> %s\n", r.payload);
}


// SCENARIO 5 — edge case 2.2.8: override manuale simultaneo a un comando del Controller

static void scenario_concurrent_override(void) {
    printf("\n=========== SCENARIO 5: override manuale simultaneo (edge case 2.2.8) ===========\n");
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
    printf("  piu' alto) perche' *_run() gestisce una connessione alla volta con\n");
    printf("  accept() bloccante: il sistema operativo mette in attesa la seconda\n");
    printf("  connessione finche' la prima non e' chiusa. Il device non riceve mai i\n");
    printf("  due comandi 'mescolati' e non finisce mai in uno stato indefinito: lo\n");
    printf("  stato finale corrisponde sempre a UNO dei due comandi per intero, mai\n");
    printf("  a una via di mezzo incoerente.\n");
}


int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    char path[NUM_DEVICES][128];
    int srv[NUM_DEVICES];
    pid_t pid[NUM_DEVICES];

    srv[BULB_ID]   = create_server(BULB_ID,   path[BULB_ID]);
    srv[WINDOW_ID] = create_server(WINDOW_ID, path[WINDOW_ID]);
    srv[FRIDGE_ID] = create_server(FRIDGE_ID, path[FRIDGE_ID]);
    srv[HUB_ID]    = create_server(HUB_ID,    path[HUB_ID]);
    srv[TIMER_ID]  = create_server(TIMER_ID,  path[TIMER_ID]);
    srv[BULB2_ID]  = create_server(BULB2_ID,  path[BULB2_ID]);

    fflush(stdout);
    pid[BULB_ID] = fork();
    if (pid[BULB_ID] == 0) { bulb_run(srv[BULB_ID], BULB_ID); exit(0); }

    fflush(stdout);
    pid[WINDOW_ID] = fork();
    if (pid[WINDOW_ID] == 0) { window_run(srv[WINDOW_ID], WINDOW_ID); exit(0); }

    fflush(stdout);
    pid[FRIDGE_ID] = fork();
    if (pid[FRIDGE_ID] == 0) { fridge_run(srv[FRIDGE_ID], FRIDGE_ID); exit(0); }

    fflush(stdout);
    pid[HUB_ID] = fork();
    if (pid[HUB_ID] == 0) { hub_run(srv[HUB_ID], HUB_ID); exit(0); }

    fflush(stdout);
    pid[TIMER_ID] = fork();
    if (pid[TIMER_ID] == 0) { timer_run(srv[TIMER_ID], TIMER_ID); exit(0); }

    fflush(stdout);
    pid[BULB2_ID] = fork();
    if (pid[BULB2_ID] == 0) { bulb_run(srv[BULB2_ID], BULB2_ID); exit(0); }

    scenario_base();
    scenario_fridge();
    scenario_hub();
    scenario_timer();
    scenario_concurrent_override();

    printf("\nfine dimostrazione\n");

    for (int i = 0; i < NUM_DEVICES; i++) {
        kill(pid[i], SIGTERM);
    }
    for (int i = 0; i < NUM_DEVICES; i++) {
        waitpid(pid[i], NULL, 0);
        remove_socket(path[i]);
    }

    return 0;
}