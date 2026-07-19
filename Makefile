CC = gcc
CFLAGS = -Iinclude

SRC = src/main.c src/controller.c src/device.c src/ipc_utils.c src/errors.c
OUT = controller

all:
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT)