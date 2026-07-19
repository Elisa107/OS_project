#include <stdio.h>
#include <../include/device.h>
#include <../include/common.h>
#include <../include/errors.h>
#include <../include/protocol.h>
#include <../include/ipc_utils.h>
#include <../include/controller.h>


int main(int argc, char *argv[]) {
    
    printf("Starting the device...\n");
    controller_shell(); // in controller.c

    return 0;
}