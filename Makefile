CC = gcc
CFLAGS = -Iinclude

# Sorgenti del Controller: include anche la logica dei device (modo A,
# i device girano dentro il processo Controller, non come eseguibili separati).
# MODIFICA (Evelin): aggiunti src/bulb.c e src/window.c in fondo alla riga
# sottostante (il resto della riga era già presente).
CONTROLLER_SRC = src/main.c src/controller.c src/device.c src/ipc_utils.c \
                 src/errors.c src/fridge.c src/hub.c src/timer.c \
                 src/bulb.c src/window.c src/signal_utils.c

DEVICE_SRC = src/ipc_utils.c src/errors.c src/signal_utils.c \
             src/bulb.c src/window.c src/fridge.c src/hub.c src/timer.c

DEMO_SRC = test/test_all_devices.c $(DEVICE_SRC)
DEMO      = test_all_devices

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

$(DEMO): $(DEMO_SRC)
	$(CC) $(CFLAGS) -o $(DEMO) $(DEMO_SRC)

demo: $(DEMO)
	./$(DEMO)

run: build
	./$(CONTROLLER)

clean:
	rm -f $(CONTROLLER) $(MANUAL) $(DEMO)
	rm -f /tmp/domotic_*.sock