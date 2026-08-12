#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "common.h"

int controller_init();
int controller_shell();
int list_devices();
int add_device(DeviceType type, int parent_id);
int info(int device_id, char *output);
int switch_device(int device_id, char *label, char *value);
int creates_cycle(int device_id, int parent_id);
int link_devices(int device_id, int parent_id);
int delete_device(int device_id);

#endif