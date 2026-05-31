#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

void handle_sigint(int signal) {
    (void)signal;
    keep_running = 0;
}

int main(void) {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    printf("Hola mundo desde C dentro de Docker!\n");
    printf("Ejecutando indefinidamente. Pulsa Ctrl+C para salir.\n");
    fflush(stdout);

    while (keep_running) {
        sleep(1);
    }

    printf("\nSaliendo correctamente. Hasta luego!\n");
    return 0;
}
