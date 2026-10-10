/**
 * huf_main.c - Herramienta independiente 'huf' para probar el compresor sin el editor.
 *
 *   ./huf c <entrada> <salida.huf> [-t hilos]     comprime
 *   ./huf d <entrada.huf> <salida> [-t hilos]     descomprime
 *
 * Ctrl+C cancela limpiamente (no deja archivos a medias).
 */

#include "huf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static void al_recibir_senal(int sig)
{
    (void)sig;
    huf_abortar_por_senal();      /* solo escribe un atomic: async-signal-safe */
}

static void progreso(void *ud, int pct, HufEstado est, const char *msg)
{
    (void)ud;
    if (est == HUF_CORRIENDO) {
        fprintf(stderr, "\r[%-20.*s%*s] %3d%%  %s\033[K", pct / 5, "####################",
                20 - pct / 5, "", pct, msg);
    } else {
        fprintf(stderr, "\r\033[K%s\n", msg);
    }
    fflush(stderr);
}

int main(int argc, char **argv)
{
    int hilos = 0;
    if (argc < 4 || (argv[1][0] != 'c' && argv[1][0] != 'd')) {
        fprintf(stderr, "Uso: %s c|d <entrada> <salida> [-t hilos]\n", argv[0]);
        return 2;
    }
    for (int i = 4; i + 1 < argc; i++)
        if (strcmp(argv[i], "-t") == 0) hilos = atoi(argv[i + 1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = al_recibir_senal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    HufJob *j = huf_job_iniciar(argv[1][0] == 'c' ? HUF_COMPRIMIR : HUF_DESCOMPRIMIR,
                                argv[2], argv[3], hilos, progreso, NULL);
    if (j == NULL) return 1;

    HufEstado e = huf_job_esperar(j);
    huf_job_liberar(j);
    return e == HUF_OK ? 0 : (e == HUF_CANCELADO ? 130 : 1);
}
