# OS_project
## Architecture
- **Controller**: entry point of the system. It exposes an
  interactive shell on stdin and owns a UNIX socket
  (`/tmp/domotic_controller.sock`) used to receive asynchronous notifications
  from devices (manual overrides).
- **Devices**: every time you `add` a device, the Controller `fork()`s a
  child process. Each child opens its own UNIX socket
  (`/tmp/domotic_<id>.sock`) and runs a loop (`select()`)
  waiting for messages from the Controller or from other devices (the control ones).
- **Control devices**: `HUB`, `TIMER`. They can have children linked to them.
- **Interaction devices**: `BULB`, `WINDOW`, `FRIDGE`. Normal devices.
- **Protocol** (`include/protocol.h`): `Message` struct used to commmunicate
  (through the sockets).
  Commands: `CMD_INFO`, `CMD_SWITCH`, `CMD_LINK`, `CMD_DELETE`, `CMD_ACK`,
  `CMD_ERROR`, `CMD_OVERRIDE`.
- **manual_interaction`**: a standalone executable that talks directly to a
  device's socket, bypassing the Controller (used to simulate someone
  physically operating a device). If the device is linked to a Hub/Timer,
  this triggers a **manual override**, which is reported back to the
  Controller asynchronously (`CMD_OVERRIDE`) and printed in the shell.

## Build
make build     # builds ./controller and ./manual_interaction
make demo      # builds and runs test/test_all_devices.c
make clean     # removes leftover sockets in /tmp

## Run
make run       # starts the controller shell

## Shell commands
`list`: lists active devices with their ID, type, role and current state
`add <device>`: creates a new device and adds into the array
`del <id>`: deletes a device (and all devices linked to it if needed)
`link <id> to <parent_id>`: links device `<id>` to a control device
`switch <id> <label> <value>`: sends a switch command to device `<id>`
`info <id>`: shows detailed info about device `<id>`
`exit`: terminates all active devices and closes the Controller

### Switch labels per device
- **Bulb**: `power` -> `0`/`1`
- **Window**: `open` / `close` -> `0`/`1`
- **Fridge**: `open`, `close` -> `0`/`1`; `perc`, `thermostat`, `delay` (settable only via `manual_interaction`)
- **Timer**: `begin`/`end` -> `HH:MM`; `switch` -> label to drive on the child; `child`/`parent` -> link management
- **Hub**: `child` / `parent` -> link management

### manual_interaction
./manual_interaction <id> info
./manual_interaction <id> switch <label> <on|off>
./manual_interaction <id> set <param> <value>


### Project structure
include/     public headers
src/         Controller, device implementations, IPC helpers, signal handling, manual_interaction
test/        test_all_devices.c test file
Makefile     build targets: build, demo, run, clean