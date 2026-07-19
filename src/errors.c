#include "../include/errors.h"

const char* error_to_string(int error_code) {
    switch (error_code) {
        case SUCCESS: return "Operazione riuscita";
        case DEVICE_NOT_FOUND: return "Device non trovato";
        case INVALID_COMMAND: return "Comando non valido";
        case LINK_FAILED: return "Collegamento fallito";
        case DEVICE_TYPE_MISMATCH: return "Tipo di device non compatibile";
        case IPC_ERROR: return "Errore di comunicazione con il device";
        case DEVICE_ALREADY_EXISTS: return "Device già esistente";
        case INVALID_ARGUMENT: return "Argomento non valido";
        case TOO_MANY_SWITCHES: return "Troppi switch per questo device";
        case NO_DEVICES: return "Nessun device presente";
        case NO_CHILDREN: return "Nessun device collegato";
        default: return "Errore sconosciuto";
    }
}