# OS_project — Domotic System Simulator

A Unix domotic (home-automation) system simulator written in C, built as an
Operating Systems course project. Each device in the "smart home" is a
separate **process**, and all communication between the Controller and the
devices happens through **UNIX domain sockets** using a small custom
request/response protocol.

## Architecture

- **Controller** (`controller`): the entry point of the system. It exposes an
  interactive shell on stdin and owns a UNIX socket
  (`/tmp/domotic_controller.sock`) used to receive asynchronous notifications
  from devices (e.g. manual overrides).
- **Devices**: every time you `add` a device, the Controller `fork()`s a
  child process. Each child opens its own UNIX socket
  (`/tmp/domotic_<id>.sock`) and runs an event loop (`select()`-based)
  waiting for messages from the Controller or from other devices.
  - **Control devices**: `HUB`, `TIMER` — can have children linked to them.
  - **Interaction devices**: `BULB`, `WINDOW`, `FRIDGE` — leaf devices, the
    ones that actually expose a physical-like state.
- **IPC protocol** (`include/protocol.h`): fixed-size `Message` struct
  (`command`, `sender`, `receiver`, `payload`) sent over the sockets.
  Commands: `CMD_INFO`, `CMD_SWITCH`, `CMD_LINK`, `CMD_DELETE`, `CMD_ACK`,
  `CMD_ERROR`, `CMD_OVERRIDE`.
- **`manual_interaction`**: a standalone executable that talks directly to a
  device's socket, bypassing the Controller (used to simulate someone
  physically operating a device). If the device is linked to a Hub/Timer,
  this triggers a **manual override**, which is reported back to the
  Controller asynchronously (`CMD_OVERRIDE`) and printed as a notification in
  the shell.

## Build

```sh
make build     # builds ./controller and ./manual_interaction
make demo      # builds and runs test/test_all_devices.c
make clean     # removes binaries and leftover sockets in /tmp
```

## Run

```sh
make run       # equivalent to: make build && ./controller
```

This starts the Controller shell:

```
Starting the device...
Available commands:
  list
  add <device_type>
  del <device_id>
  link <device_id> to <parent_id>
  switch <device_id> <label> <pos>
  info <device_id>
  exit
>
```

## Shell commands

| Command | Description |
|---|---|
| `list` | Lists all active devices with their ID, type, role and current state. |
| `add <type>` | Creates a new device. `<type>` is one of `hub`, `timer`, `bulb`, `window`, `fridge`. |
| `del <id>` | Deletes a device (and recursively all devices linked to it). |
| `link <id> to <parent_id>` | Links device `<id>` to a control device (`hub`/`timer`) `<parent_id>`. |
| `switch <id> <label> <value>` | Sends a switch command to device `<id>` (see labels below). |
| `info <id>` | Shows detailed info about device `<id>`. |
| `exit` | Terminates all active devices and closes the Controller. |

### Switch labels per device

- **Bulb**: `power` -> `0`/`1`
- **Window**: `open` / `close` -> `0`/`1`
- **Fridge**: `open`, `close` -> `0`/`1`; `perc`, `thermostat`, `delay` (settable only via `manual_interaction`)
- **Timer**: `begin`/`end` -> `HH:MM`, `switch` -> label to drive on the child, `child`/`parent` -> link management
- **Hub**: `child` / `parent` -> link management

### `manual_interaction`

```sh
./manual_interaction <id> info
./manual_interaction <id> switch <label> <on|off>
./manual_interaction <id> set <param> <value>
```

## Project structure

```
include/     public headers (protocol, devices, IPC, errors, ...)
src/         Controller, device implementations (bulb, window, fridge, hub,
             timer), IPC helpers, signal handling, manual_interaction
test/        test_all_devices.c - standalone test/demo harness
Makefile     build targets: build, demo, run, clean
```