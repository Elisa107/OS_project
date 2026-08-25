#include <stdio.h>
#include <../include/device.h>
#include <../include/common.h>
#include <../include/errors.h>
#include <../include/protocol.h>
#include <../include/ipc_utils.h>
#include <../include/controller.h>


int main(int argc, char *argv[]) {

    setvbuf(stdout, NULL, _IONBF, 0);
    
    printf("Starting the device...\n");
    printf("Available commands:\n");
    printf("  list\n");
    printf("  add <device_type>\n");
    printf("  del <device_id>\n");
    printf("  link <device_id> to <parent_id>\n");
    printf("  switch <device_id> <label> <pos>\n");
    printf("  info <device_id>\n");
    printf("  exit\n");
    
    controller_shell(); // in controller.c

    return 0;
}