/**
 * ====================================================================================
 *  editor_huf.c  -  INTEGRACION DEL COMPRESOR HUFFMAN CON EL EDITOR (Parcial 2)
 * ====================================================================================
 *  Comandos nuevos del REPL (todos NO bloqueantes: el prompt vuelve de inmediato):
 *    k [salida]            Comprime el archivo abierto (por defecto -> <archivo>.huf)
 *    u <archivo.huf> [sal] Descomprime (por defecto quita la extension .huf)
 *    j                     Muestra el progreso del trabajo actual
 *    n                     Cancela el trabajo actual
 *
 *  CONDICIONES DE CARRERA CON LA EDICION
 *    El editor persiste cada edicion de inmediato sobre el mismo inodo (no hay buffer
 *    sin guardar). Si el usuario modificara el archivo mientras un hilo lo comprime, el
 *    resultado mezclaria dos versiones. Por eso los comandos que ESCRIBEN (a, i, d, x)
 *    llaman a ed_huf_puede_escribir(), que consulta al job bajo su mutex y compara
 *    (st_dev, st_ino) del fd abierto contra el origen y el destino del trabajo:
 *      - Lecturas (p, s, y, m, c): SIEMPRE permitidas (lectura compartida).
 *      - Escrituras sobre el archivo en uso: rechazadas con un mensaje claro.
 *    No hay TOCTOU: el unico hilo que puede lanzar un job es el del REPL, que en ese
 *    momento no esta ejecutando una edicion; y el job pasa a CORRIENDO ANTES de crear
 *    su hilo, asi que el candado rige desde el primer instante.
 *    Como defensa adicional (modificaciones externas, p. ej. otro programa) el job
 *    compara tamano y mtime al terminar y aborta sin dejar un .huf corrupto.
 *
 *  SALIDA POR PANTALLA DESDE OTRO HILO
 *    El hilo coordinador imprime avisos mientras el hilo principal espera en fgets().
 *    Un mutex de E/S serializa los avisos y, tras cada uno, se re-dibuja el prompt para
 *    que la pantalla no quede "rota" (lo que el usuario ya tecleo sigue en el buffer del
 *    terminal).
 * ====================================================================================
 */

#include "shell.h"
#include "editor.h"
#include "huf.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---------------------------- Prompt compartido --------------------------------- */

static pthread_mutex_t g_io = PTHREAD_MUTEX_INITIALIZER;
static char            g_prompt[PATH_MAX + 64];

void ed_prompt_mostrar(const char *p)
{
    pthread_mutex_lock(&g_io);
    snprintf(g_prompt, sizeof(g_prompt), "%s", p);
    fputs(g_prompt, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_io);
}

void ed_prompt_ocultar(void)
{
    pthread_mutex_lock(&g_io);
    g_prompt[0] = '\0';                 /* mientras se ejecuta un comando no se re-dibuja */
    pthread_mutex_unlock(&g_io);
}

/* ------------------------------- Ctrl+C ----------------------------------------- */

static struct sigaction g_sigint_previo;

static void al_sigint(int s)
{
    (void)s;
    huf_abortar_por_senal();            /* atomic_store: async-signal-safe */
}

static void sigint_instalar(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = al_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;           /* fgets() del REPL no debe fallar con EINTR */
    sigaction(SIGINT, &sa, &g_sigint_previo);
}

static void sigint_restaurar(void)
{
    sigaction(SIGINT, &g_sigint_previo, NULL);
}

/* ------------------------------- Progreso --------------------------------------- */

static int g_ultimo_decil = -1;

static void barra(char *dst, int pct)
{
    int llenos = pct / 5;
    for (int i = 0; i < 20; i++) dst[i] = (i < llenos) ? '#' : '.';
    dst[20] = '\0';
}

/* Corre en el hilo coordinador. Nunca se llama con un mutex del job tomado. */
static void progreso_cb(void *ud, int pct, HufEstado est, const char *msg)
{
    (void)ud;
    char b[21];
    barra(b, pct);

    if (est != HUF_CORRIENDO) sigint_restaurar();

    pthread_mutex_lock(&g_io);
    if (est == HUF_CORRIENDO) {
        int decil = pct / 10;           /* un aviso cada 10% para no inundar la terminal */
        if (decil == g_ultimo_decil) { pthread_mutex_unlock(&g_io); return; }
        g_ultimo_decil = decil;
        printf("\r\033[K" COLOR_INFO "[huf] [%s] %3d%%  %s" COLOR_RESET "\n", b, pct, msg);
    } else if (est == HUF_OK) {
        printf("\r\033[K" COLOR_RESULT "[huf] [%s] 100%%  %s" COLOR_RESET "\n", b, msg);
    } else if (est == HUF_CANCELADO) {
        printf("\r\033[K" COLOR_PARAM "[huf] %s" COLOR_RESET "\n", msg);
    } else {
        printf("\r\033[K" COLOR_ERROR "[huf] ERROR: %s" COLOR_RESET "\n", msg);
    }
    if (g_prompt[0]) fputs(g_prompt, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&g_io);
}

/* --------------------------- Ciclo de vida del job ------------------------------ */

static int liberar_si_termino(Editor *ed)
{
    if (ed->huf == NULL) return 0;
    if (huf_job_estado(ed->huf, NULL, NULL, 0) == HUF_CORRIENDO) return 1;
    huf_job_liberar(ed->huf);           /* ya termino: join inmediato */
    ed->huf = NULL;
    return 0;
}

static int lanzar(Editor *ed, HufModo modo, const char *entrada, const char *salida)
{
    if (liberar_si_termino(ed)) {
        fprintf(stderr, COLOR_ERROR "Ya hay una operacion en curso. Usa 'j' para ver el progreso o 'n' para cancelar.\n" COLOR_RESET);
        return -1;
    }
    printf(COLOR_RESULT "%s '%s' -> '%s' en segundo plano. Puedes seguir editando (lecturas)."
           " 'j' progreso, 'n' cancelar.\n" COLOR_RESET,
           modo == HUF_COMPRIMIR ? "Comprimiendo" : "Descomprimiendo", entrada, salida);
    fflush(stdout);

    g_ultimo_decil = -1;
    sigint_instalar();                  /* ANTES de crear el hilo: el callback final lo restaura */
    ed->huf = huf_job_iniciar(modo, entrada, salida, 0, progreso_cb, NULL);
    if (ed->huf == NULL) {
        sigint_restaurar();
        return -1;
    }
    return 0;
}

/* ------------------------------ Comandos ---------------------------------------- */

int ed_cmd_comprimir(Editor *ed, const char *resto)
{
    char salida[PATH_MAX + 8];
    if (*resto != '\0') snprintf(salida, sizeof(salida), "%s", resto);
    else                snprintf(salida, sizeof(salida), "%s.huf", ed->ruta);
    return lanzar(ed, HUF_COMPRIMIR, ed->ruta, salida);
}

int ed_cmd_descomprimir(Editor *ed, const char *resto)
{
    char entrada[PATH_MAX], salida[PATH_MAX + 8];
    entrada[0] = salida[0] = '\0';
    int n = sscanf(resto, "%4095s %4095s", entrada, salida);
    if (n < 1) {
        fprintf(stderr, COLOR_ERROR "Uso: u <archivo.huf> [salida]\n" COLOR_RESET);
        return -1;
    }
    if (n == 1) {
        size_t l = strlen(entrada);
        if (l > 4 && strcmp(entrada + l - 4, ".huf") == 0) snprintf(salida, sizeof(salida), "%.*s", (int)(l - 4), entrada);
        else                                               snprintf(salida, sizeof(salida), "%s.out", entrada);
    }
    if (access(salida, F_OK) == 0) {     /* no pisar un archivo existente sin avisar */
        fprintf(stderr, COLOR_ERROR "'%s' ya existe. Indica otro destino: u %s <salida>\n" COLOR_RESET, salida, entrada);
        return -1;
    }
    return lanzar(ed, HUF_DESCOMPRIMIR, entrada, salida);
}

void ed_cmd_estado_huf(Editor *ed)
{
    if (ed->huf == NULL) {
        printf(COLOR_INFO "No hay ninguna operacion de compresion en curso.\n" COLOR_RESET);
        return;
    }
    char msg[320], b[21];
    int pct = 0;
    HufEstado e = huf_job_estado(ed->huf, &pct, msg, sizeof(msg));
    barra(b, pct);
    if (e == HUF_CORRIENDO) printf(COLOR_INFO "[huf] [%s] %3d%%  %s\n" COLOR_RESET, b, pct, msg);
    else                    printf(COLOR_INFO "[huf] ultimo trabajo: %s\n" COLOR_RESET, msg);
    liberar_si_termino(ed);
}

void ed_cmd_cancelar_huf(Editor *ed)
{
    if (ed->huf == NULL || huf_job_estado(ed->huf, NULL, NULL, 0) != HUF_CORRIENDO) {
        printf(COLOR_INFO "No hay ninguna operacion en curso para cancelar.\n" COLOR_RESET);
        return;
    }
    huf_job_cancelar(ed->huf);          /* no bloquea: el aviso final lo imprime el job */
}

/* Guardia de escritura: 1 = se puede editar; 0 = bloqueado (ya imprimio el motivo). */
int ed_huf_puede_escribir(Editor *ed)
{
    if (ed->huf == NULL || !ed->abierto) return 1;
    if (!huf_job_bloquea_fd(ed->huf, ed->fd)) return 1;
    int pct = 0;
    huf_job_estado(ed->huf, &pct, NULL, 0);
    fprintf(stderr, COLOR_ERROR "Archivo bloqueado: hay una compresion/descompresion en curso (%d%%) que usa "
            "'%s'. Las lecturas siguen disponibles; espera ('j') o cancela ('n').\n" COLOR_RESET, pct, ed->ruta);
    return 0;
}

/* Al salir: cancela si sigue vivo, hace join de todos los hilos y libera. */
void ed_huf_cerrar(Editor *ed)
{
    if (ed->huf == NULL) return;
    if (huf_job_estado(ed->huf, NULL, NULL, 0) == HUF_CORRIENDO)
        printf(COLOR_INFO "Cancelando la operacion en curso antes de salir...\n" COLOR_RESET);
    huf_job_liberar(ed->huf);
    ed->huf = NULL;
}
