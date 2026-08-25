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
#define BULB2_ID   5  // second bulb, used by timer

#define NUM_DEVICES 6

static void req(int id, int sender, Command cmd, const char *payload, Message *out) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/domotic_%d.sock", id);

    int fd;
    for (int i = 0; i < 100 && (fd = connect_device(path)) == -1; i++){
        usleep(20000);
    }
    if (fd == -1){ 
        fprintf(stderr, "can't connect to device %d\n", id); 
        exit(1); 
    }

    Message m; memset(&m, 0, sizeof m);
    m.command = cmd; m.sender = sender; m.receiver = id;
    if (payload){
        strncpy(m.payload, payload, sizeof m.payload - 1);
    }

    send_message(fd, &m);
    receive_message(fd, out);
    close_connection(fd);
}


// 1: Bulb and Window

static void scenario_base(void) {
    Message r;
    printf("\n=== 1: Bulb/Window ===\n");
    printf("\n-- BULB (id=%d) --\n", BULB_ID);
    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info               -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  power:1            -> %s\n", r.payload);

    sleep(2);
    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info after 2s      -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "power:0", &r);
    printf("  power:0            -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("  info after off     -> %s\n", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "power:5", &r);
    printf("  power:5 (bad val)  -> cmd=%s payload=%s\n",
        r.command == CMD_ERROR ? "CMD_ERROR" : "???", r.payload);

    req(BULB_ID, 0, CMD_SWITCH, "pluto:1", &r);
    printf("  pluto:1 (bad label)-> cmd=%s payload=%s\n",
        r.command == CMD_ERROR ? "CMD_ERROR" : "???", r.payload);

    printf("\n-- WINDOW (id=%d) --\n", WINDOW_ID);
    req(WINDOW_ID, 0, CMD_INFO, NULL, &r);
    printf("  info               -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_SWITCH, "open:1", &r);
    printf("  open:1             -> %s\n", r.payload);

    sleep(2);
    req(WINDOW_ID, 0, CMD_INFO, NULL, &r);
    printf("  info after 2s      -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_SWITCH, "open:0", &r);
    printf("  open:0 (should be ignored) -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_SWITCH, "close:1", &r);
    printf("  close:1            -> %s\n", r.payload);

    req(WINDOW_ID, 0, CMD_INFO, NULL, &r);
    printf("  info after close   -> %s\n", r.payload);
}


// 2: Fridge (delay, perc, thermostat, auto-close)

static void scenario_fridge(void) {
    Message r;
    printf("\n=== 2: Fridge ===\n");

    req(FRIDGE_ID, 0, CMD_INFO, NULL, &r);
    printf("  info               -> %s\n", r.payload);

    req(FRIDGE_ID, -1, CMD_SWITCH, "perc:40", &r);
    printf("  perc:40 (manual)   -> %s\n", r.payload);

    req(FRIDGE_ID, -1, CMD_SWITCH, "thermostat:3", &r);
    printf("  thermostat:3 (manual) -> %s\n", r.payload);

    req(FRIDGE_ID, -1, CMD_SWITCH, "delay:2", &r);
    printf("  delay:2            -> %s\n", r.payload);

    req(FRIDGE_ID, 0, CMD_SWITCH, "open:1", &r);
    printf("  open:1             -> %s\n", r.payload);

    req(FRIDGE_ID, 0, CMD_INFO, NULL, &r);
    printf("  info right after   -> %s\n", r.payload);

    printf("  waiting 3s (delay was 2s)...\n");
    sleep(3);

    req(FRIDGE_ID, 0, CMD_INFO, NULL, &r);
    printf("  info after timeout -> %s (expected: closed)\n", r.payload);
}


// 3 Hub: propagation + manual override detection

static void scenario_hub(void) {
    Message r;
    printf("\n== 3: Hub ===\n");
    req(HUB_ID, 0, CMD_LINK, "child:0", &r);
    printf("  link bulb(%d)       -> %s\n", BULB_ID, r.payload);

    req(HUB_ID, 0, CMD_LINK, "child:1", &r);
    printf("  link window(%d)     -> %s\n", WINDOW_ID, r.payload);

    req(HUB_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  hub power:1        -> %s\n", r.payload);

    req(HUB_ID, 0, CMD_INFO, NULL, &r);
    printf("  hub info           -> %s (expected: on, no override)\n", r.payload);

    printf("  manually switching bulb only, bypassing hub...\n");
    req(BULB_ID, -1, CMD_SWITCH, "power:0", &r);
    printf("  bulb power:0 (manual) -> %s\n", r.payload);

    req(HUB_ID, 0, CMD_INFO, NULL, &r);
    printf("  hub info           -> %s (expected: manual override)\n", r.payload);

    req(HUB_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  hub power:1 again  -> %s\n", r.payload);

    req(HUB_ID, 0, CMD_INFO, NULL, &r);
    printf("  hub info           -> %s (expected: consistent again)\n", r.payload);
}


// 4 Timer: linked child + begin/end validation

static void scenario_timer(void) {
    Message r;

    printf("\n=== 4: Timer ===\n");

    req(TIMER_ID, 0, CMD_LINK, "child:5", &r);
    printf("  link bulb2(%d)      -> %s\n", BULB2_ID, r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "begin:08:00", &r);
    printf("  begin:08:00        -> %s\n", r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "end:07:00", &r);
    printf("  end:07:00 (before begin) -> cmd=%s payload=%s\n",
        r.command == CMD_ERROR ? "CMD_ERROR" : "???", r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "end:20:00", &r);
    printf("  end:20:00          -> %s\n", r.payload);

    req(TIMER_ID, 0, CMD_SWITCH, "power:1", &r);
    printf("  power:1 (propagate)-> %s\n", r.payload);

    req(TIMER_ID, 0, CMD_INFO, NULL, &r);
    printf("  timer info         -> %s\n", r.payload);

    req(BULB2_ID, 0, CMD_INFO, NULL, &r);
    printf("  bulb2 info direct  -> %s\n", r.payload);
}


// 5 edge case 2.2.8: manual override vs Controller command, same time
static void scenario_concurrent_override(void) {
    printf("\n=== 5: concurrent override (2.2.8) ==\n");
    printf("resetting bulb to off...\n");
    Message r;
    req(BULB_ID, 0, CMD_SWITCH, "power:0", &r);

    printf("sending two commands to bulb(%d) at the same time:\n", BULB_ID);
    printf("one from Controller, one manual\n\n");

    fflush(stdout);
    pid_t p1 = fork();
    if (p1 == 0){
        Message out;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        req(BULB_ID, 0, CMD_SWITCH, "power:1", &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("  [controller] power:1 -> %-12s (%.2fs)\n", out.payload, elapsed);
        exit(0);
    }

    fflush(stdout);
    pid_t p2 = fork();
    if (p2 == 0){
        Message out;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        req(BULB_ID, -1, CMD_SWITCH, "power:0", &out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("  [manual]     power:0 -> %-12s (%.2fs)\n", out.payload, elapsed);
        exit(0);
    }

    int status;
    waitpid(p1, &status, 0);
    waitpid(p2, &status, 0);

    req(BULB_ID, 0, CMD_INFO, NULL, &r);
    printf("\n  final bulb state -> %s\n", r.payload);
    printf("  one command waits for the other, accept() is blocking so they\n");
    printf("  never interleave - bulb ends up fully in one state or the other.\n");
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
    if (pid[BULB_ID] == 0) { 
        bulb_run(srv[BULB_ID], BULB_ID);
        exit(0);
    }

    fflush(stdout);
    pid[WINDOW_ID] = fork();
    if (pid[WINDOW_ID] == 0){ 
        window_run(srv[WINDOW_ID], WINDOW_ID); 
        exit(0); 
    }

    fflush(stdout);
    pid[FRIDGE_ID] = fork();
    if (pid[FRIDGE_ID] == 0) {
        fridge_run(srv[FRIDGE_ID], FRIDGE_ID);
        exit(0); 
    }

    fflush(stdout);
    pid[HUB_ID] = fork();
    if (pid[HUB_ID] == 0){ 
        hub_run(srv[HUB_ID], HUB_ID);
        exit(0);
    }

    fflush(stdout);
    pid[TIMER_ID] = fork();
    if (pid[TIMER_ID] == 0){ 
        timer_run(srv[TIMER_ID], TIMER_ID);
        exit(0);
    }

    fflush(stdout);
    pid[BULB2_ID] = fork();
    if (pid[BULB2_ID] == 0) {
        bulb_run(srv[BULB2_ID], BULB2_ID);
        exit(0); 
    }

    scenario_base();
    scenario_fridge();
    scenario_hub();
    scenario_timer();
    scenario_concurrent_override();

    printf("\ndone\n");

    for (int i = 0; i < NUM_DEVICES; i++){
        kill(pid[i], SIGTERM);
    }
    for (int i = 0; i < NUM_DEVICES; i++) {
        waitpid(pid[i], NULL, 0);
        remove_socket(path[i]);
    }

    return 0;
}