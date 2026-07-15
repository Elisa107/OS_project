#include <stdio.h>
#include <../include/device.h>
#include <../include/common.h>
#include <../include/errors.h>
#include <../include/protocol.h>
#include <../include/ipc_utils.h>


int main(int argc, char *argv[]) {
    
    printf("Starting the device...\n");
    run_controller(); // in controller.c

    return 0;
}