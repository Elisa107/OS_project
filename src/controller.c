#include "../include/common.h"
#include "../include/errors.h"
#include "../include/protocol.h"
#include "../include/ipc_utils.h"
#include "../include/device.h"

int next_id = 0; // keep track of the next device ID to assign
Device devices[MAX_DEVICES];
int device_count = 0;

int controller_init(){
    

}

int controller_shell(){

}

int list_devices(){
    if (device_count == 0) {
        return NO_DEVICES;
    }
    for(int i = 0; i < device_count; i++) {
        // take everything from the struct
        printf("Device ID: %d\n", devices[i].id);
        printf("Type: %s\n", type_to_string(devices[i].type));
        printf("Role: %s\n", is_control_device(devices[i].type) ? "Control" : "Interaction");
    }
    return SUCCESS;
}

int add_device(DeviceType type, int parent_id){

}

int info(int device_id){
    for(int i = 0; i < device_count; i++) {
        if(devices[i].id == device_id) {
            // take the first 2 from the struct
            printf("Device ID: %d\n", devices[i].id);
            printf("Type: %s\n", type_to_string(devices[i].type));
            printf("Role: %s\n", is_control_device(devices[i].type) ? "Control" : "Interaction");

            int fd = connect_device(devices[i].socket_path);
            if (fd == -1) {
                return IPC_ERROR;
            }

            // take this from IPC
            Message request;
            request.command = CMD_INFO;
            request.sender = 0;         // or the controller's ID if you have one
            request.receiver = device_id;

            send_message(fd, &request);

            Message response;
            receive_message(fd, &response);

            printf("State: %s\n", response.payload);  // the device write here his state

            close_connection(fd);
            return SUCCESS;
        }
    }
    return DEVICE_NOT_FOUND;
}

void switch_device(int device_id, char* label, int switch_pos){

}

int link_devices(int device_id, int parent_id){

}

int delete_device(int device_id){
    if (device_count == 0) {
        printf("No devices to delete.\n");
        return DEVICE_NOT_FOUND;
    }
    if(device_id < 0 || device_id >= next_id) {
        printf("Invalid device ID: %d\n", device_id);
        return DEVICE_NOT_FOUND;
    }
    // Find the device to delete
    // delete the device and close its socket connection
    return SUCCESS;
}


