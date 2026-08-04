CC = gcc
CFLAGS = -Iinclude

# Sorgenti del Controller: include anche la logica dei device (modo A,
# i device girano dentro il processo Controller, non come eseguibili separati).
CONTROLLER_SRC = src/main.c src/controller.c src/device.c src/ipc_utils.c \
                 src/errors.c src/fridge.c src/hub.c src/timer.c

# manual_interaction e' un eseguibile a parte (ha un suo main).
MANUAL_SRC = src/manual_interaction.c src/ipc_utils.c

CONTROLLER = controller
MANUAL     = manual_interaction

.PHONY: build all clean run

build: $(CONTROLLER) $(MANUAL)
all: build

$(CONTROLLER): $(CONTROLLER_SRC)
	$(CC) $(CFLAGS) -o $(CONTROLLER) $(CONTROLLER_SRC)

$(MANUAL): $(MANUAL_SRC)
	$(CC) $(CFLAGS) -o $(MANUAL) $(MANUAL_SRC)

run: build
	./$(CONTROLLER)

clean:
	rm -f $(CONTROLLER) $(MANUAL)
	rm -f /tmp/domotic_*.sock