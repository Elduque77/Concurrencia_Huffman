/**
 * ====================================================================================
 *  huf_job.c  -  PIPELINE CONCURRENTE DE COMPRESION / DESCOMPRESION HUFFMAN
 * ====================================================================================
 *  HILOS Y ROLES
 *    - Hilo coordinador (1): creado por huf_job_iniciar(). Orquesta las fases, crea el
 *      pool, reporta progreso y, en la fase de escritura, actua como ESCRITOR ORDENADO.
 *    - Hilos trabajadores (N): leen bloques con pread(2) y los procesan en paralelo.
 *
 *  FASES (compresion)
 *    1. CONTEO     : cada worker toma bloques de un contador compartido y cuenta en su
 *                    propia tabla local (reduccion local, sin contencion por byte). Al
 *                    terminar fusiona su tabla con UN solo lock.
 *    2. ARBOL      : el coordinador construye el codigo (secuencial, 256 simbolos).
 *    3. CODIFICACION + ESCRITURA: los workers codifican bloques en paralelo y dejan el
 *                    resultado en results[idx]; el coordinador los escribe EN ORDEN.
 *  Descompresion: escaneo de cabeceras de bloque + la misma fase 3 (decodificar).
 *
 *  MECANISMOS DE SINCRONIZACION (un unico mutex 'lock' protege todo el estado mutable)
 *    cv_trabajo : los workers esperan aqui cuando la VENTANA esta llena (no pueden
 *                 adelantarse mas de 'ventana' bloques al escritor => memoria acotada).
 *    cv_listo   : el coordinador espera aqui a que un bloque quede listo / avance el
 *                 conteo / ocurra un error o una cancelacion.
 *    cv_fin     : quien llama a huf_job_esperar() duerme aqui hasta que termine el job.
 *
 *  ARGUMENTOS DE AUSENCIA DE PROBLEMAS
 *    - Sin espera activa: toda espera es pthread_cond_wait / timedwait (duerme).
 *      El timedwait de 200 ms existe SOLO para observar la bandera de senal, porque
 *      pthread_cond_broadcast no es async-signal-safe y el manejador no puede despertar.
 *    - Sin interbloqueo: hay un solo mutex (no hay orden de locks que violar); nunca se
 *      hace E/S, malloc de bloques ni callback con el mutex tomado; el bloque
 *      'sig_escribir' siempre esta asignado a algun worker que lo terminara sin esperar
 *      a nadie, asi que la ventana nunca bloquea el progreso.
 *    - Sin condiciones de carrera: todo campo mutable se toca bajo 'lock'. Lo demas
 *      (tabla de codigos, metadatos, rutas) se escribe ANTES de pthread_create y despues
 *      es de solo lectura (pthread_create/join establecen la relacion happens-before).
 * ====================================================================================
 */

#include "huf.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ESPERA_MS      200
#define MAX_HILOS      32
#define BLOCK_SIZE_MAX (1u << 24)

static atomic_int g_abort = 0;
void huf_abortar_por_senal(void) { atomic_store(&g_abort, 1); }

/* ------------------------------ Estructuras ------------------------------------- */

typedef struct {
    uint8_t *data;
    uint32_t size;       /* compresion: bytes comprimidos; descompresion: bytes originales */
    uint32_t orig_len;
    uint32_t crc;
    int      listo;
} BlockResult;

typedef struct {
    uint64_t off;        /* offset de la cabecera del bloque dentro del .huf */
    uint32_t orig_len, comp_len, crc;
} BlockMeta;

struct HufJob {
    /* --- Configuracion: inmutable despues de iniciar --- */
    HufModo       modo;
    char          ruta_in[PATH_MAX], ruta_out[PATH_MAX], ruta_tmp[PATH_MAX + 8];
    int           n_hilos;
    HufProgresoCb cb;
    void         *ud;
    int           fd_in, fd_out;
    dev_t         in_dev, out_dev;
    ino_t         in_ino, out_ino;
    int           out_previo;               /* 1 si el destino ya existia              */
    struct stat   st_ini;                   /* para detectar cambios del origen        */
    uint32_t      block_size;
    uint32_t      n_bloques;
    uint64_t      orig_size;
    uint32_t      ventana;
    HufTabla      tabla;                    /* solo lectura tras construirse           */
    BlockMeta    *meta;                     /* descompresion, solo lectura             */
    BlockResult  *res;                      /* resultados por bloque (bajo lock)       */
    uint64_t      bytes_in, bytes_out;      /* solo los toca el coordinador            */

    /* --- Estado compartido: SIEMPRE bajo 'lock' --- */
    pthread_mutex_t lock;
    pthread_cond_t  cv_trabajo, cv_listo, cv_fin;
    HufEstado   estado;
    int         cancelado, error, terminado, coord_vivo;
    char        msg[320];
    const char *fase;
    uint32_t    sig_asignar, sig_escribir, contados, escritos;
    uint64_t    freq_global[256];
    int         pct_reportado;

    pthread_t   hilo_coord;
};

/* ------------------------------ Utilidades -------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* pread(2) completo: tolera lecturas cortas y EINTR. EOF prematuro => errno = EIO. */
static int leer_en(int fd, void *buf, size_t n, off_t off)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = EIO; return -1; }
        got += (size_t)r;
    }
    return 0;
}

static int escribir_todo(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)w;
    }
    return 0;
}

static void esperar_cv(HufJob *j, pthread_cond_t *cv)   /* con el lock tomado */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += (long)ESPERA_MS * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(cv, &j->lock, &ts);
}

static int cancelado_locked(HufJob *j)
{
    return j->cancelado || atomic_load(&g_abort);
}

static void despertar_todos_locked(HufJob *j)
{
    pthread_cond_broadcast(&j->cv_trabajo);
    pthread_cond_broadcast(&j->cv_listo);
}

/* Registra el PRIMER error y despierta a todos. Captura errno antes de formatear. */
static void fallar(HufJob *j, const char *fmt, ...)
{
    int e = errno;
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&j->lock);
    if (!j->error) {
        j->error = 1;
        if (e) snprintf(j->msg, sizeof(j->msg), "%s: %s", tmp, strerror(e));
        else   snprintf(j->msg, sizeof(j->msg), "%s", tmp);
    }
    despertar_todos_locked(j);
    pthread_mutex_unlock(&j->lock);
}

static uint32_t tam_bloque(const HufJob *j, uint32_t idx)
{
    uint64_t ini = (uint64_t)idx * j->block_size;
    uint64_t rest = j->orig_size - ini;
    return (uint32_t)(rest < j->block_size ? rest : j->block_size);
}

static int pct_locked(HufJob *j)
{
    if (j->estado == HUF_OK) return 100;
    uint64_t total, hecho;
    if (j->modo == HUF_COMPRIMIR) { total = 2ull * j->n_bloques; hecho = (uint64_t)j->contados + j->escritos; }
    else                          { total = j->n_bloques;        hecho = j->escritos; }
    if (total == 0) return 0;
    int p = (int)(hecho * 100 / total);
    return p > 99 ? 99 : p;
}

/* Solo lo llama el coordinador. El callback corre SIN ningun mutex del job tomado. */
static void emitir(HufJob *j, int forzar)
{
    char msg[320];
    pthread_mutex_lock(&j->lock);
    int p = pct_locked(j);
    HufEstado est = j->estado;
    if (!forzar && p == j->pct_reportado) { pthread_mutex_unlock(&j->lock); return; }
    j->pct_reportado = p;
    if (est == HUF_CORRIENDO) snprintf(msg, sizeof(msg), "%s", j->fase ? j->fase : "");
    else                      snprintf(msg, sizeof(msg), "%s", j->msg);
    pthread_mutex_unlock(&j->lock);
    if (j->cb) j->cb(j->ud, p, est, msg);
}

/* ------------------------------ Fase 1: conteo ---------------------------------- */

static void *hilo_contar(void *arg)
{
    HufJob *j = (HufJob *)arg;
    uint64_t local[256];
    memset(local, 0, sizeof(local));
    uint8_t *buf = (uint8_t *)malloc(j->block_size);
    if (buf == NULL) { errno = ENOMEM; fallar(j, "memoria"); }

    while (buf != NULL) {
        pthread_mutex_lock(&j->lock);
        if (cancelado_locked(j) || j->error || j->sig_asignar >= j->n_bloques) {
            pthread_mutex_unlock(&j->lock);
            break;
        }
        uint32_t idx = j->sig_asignar++;
        pthread_mutex_unlock(&j->lock);

        uint32_t n = tam_bloque(j, idx);
        if (leer_en(j->fd_in, buf, n, (off_t)idx * j->block_size) < 0) {
            fallar(j, "lectura del bloque %u (el archivo cambio o esta truncado)", idx);
            break;
        }
        for (uint32_t i = 0; i < n; i++) local[buf[i]]++;

        pthread_mutex_lock(&j->lock);
        j->contados++;
        pthread_cond_broadcast(&j->cv_listo);
        pthread_mutex_unlock(&j->lock);
    }
    free(buf);

    pthread_mutex_lock(&j->lock);                /* reduccion: un solo lock por worker */
    for (int s = 0; s < 256; s++) j->freq_global[s] += local[s];
    pthread_mutex_unlock(&j->lock);
    return NULL;
}

/* ------------------------ Fase 3: codificar / decodificar ----------------------- */

static void *hilo_proceso(void *arg)
{
    HufJob *j = (HufJob *)arg;
    uint8_t *buf = NULL;

    if (j->modo == HUF_COMPRIMIR) {
        buf = (uint8_t *)malloc(j->block_size);
        if (buf == NULL) { errno = ENOMEM; fallar(j, "memoria"); return NULL; }
    }

    for (;;) {
        pthread_mutex_lock(&j->lock);
        /* VENTANA: no adelantarse mas de 'ventana' bloques al escritor. */
        while (!cancelado_locked(j) && !j->error && j->sig_asignar < j->n_bloques &&
               j->sig_asignar >= j->sig_escribir + j->ventana)
            pthread_cond_wait(&j->cv_trabajo, &j->lock);
        if (cancelado_locked(j) || j->error || j->sig_asignar >= j->n_bloques) {
            pthread_mutex_unlock(&j->lock);
            break;
        }
        uint32_t idx = j->sig_asignar++;
        pthread_mutex_unlock(&j->lock);

        BlockResult r;
        memset(&r, 0, sizeof(r));

        if (j->modo == HUF_COMPRIMIR) {
            uint32_t n = tam_bloque(j, idx);
            size_t   osz = 0;
            if (leer_en(j->fd_in, buf, n, (off_t)idx * j->block_size) < 0) {
                fallar(j, "lectura del bloque %u (el archivo cambio o esta truncado)", idx);
                break;
            }
            r.crc = huf_crc32(buf, n);
            r.data = huf_codificar_bloque(&j->tabla, buf, n, &osz);
            if (r.data == NULL) { errno = 0; fallar(j, "no se pudo codificar el bloque %u", idx); break; }
            r.orig_len = n;
            r.size = (uint32_t)osz;
        } else {
            const BlockMeta *m = &j->meta[idx];
            uint8_t *cbuf = (uint8_t *)malloc(m->comp_len ? m->comp_len : 1);
            uint8_t *obuf = (uint8_t *)malloc(m->orig_len ? m->orig_len : 1);
            if (cbuf == NULL || obuf == NULL) {
                free(cbuf); free(obuf);
                errno = ENOMEM; fallar(j, "memoria"); break;
            }
            if (leer_en(j->fd_in, cbuf, m->comp_len, (off_t)(m->off + HUF_BLK_HDR)) < 0) {
                free(cbuf); free(obuf);
                fallar(j, "lectura del bloque %u", idx); break;
            }
            int rc = huf_decodificar_bloque(&j->tabla, cbuf, m->comp_len, obuf, m->orig_len);
            free(cbuf);
            if (rc < 0 || huf_crc32(obuf, m->orig_len) != m->crc) {
                free(obuf);
                errno = 0; fallar(j, "bloque %u corrupto (CRC o datos invalidos)", idx); break;
            }
            r.data = obuf; r.size = m->orig_len; r.orig_len = m->orig_len; r.crc = m->crc;
        }

        r.listo = 1;
        pthread_mutex_lock(&j->lock);
        j->res[idx] = r;
        pthread_cond_broadcast(&j->cv_listo);
        pthread_mutex_unlock(&j->lock);
    }
    free(buf);
    return NULL;
}

/* ----------------------------- Coordinador -------------------------------------- */

/* Crea 'n' hilos; devuelve cuantos pudo crear. */
static int lanzar_pool(HufJob *j, pthread_t *th, int n, void *(*fn)(void *))
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, fn, j) != 0) break;
        ok++;
    }
    return ok;
}

static void unir_pool(pthread_t *th, int n)
{
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
}

/* Si llego una senal, convierte la bandera global en cancelacion del job. */
static void revisar_senal(HufJob *j)
{
    if (atomic_load(&g_abort)) {
        pthread_mutex_lock(&j->lock);
        j->cancelado = 1;
        despertar_todos_locked(j);
        pthread_mutex_unlock(&j->lock);
    }
}

static int cancelado_o_error(HufJob *j)
{
    pthread_mutex_lock(&j->lock);
    int r = cancelado_locked(j) || j->error;
    pthread_mutex_unlock(&j->lock);
    return r;
}

static void fase_texto(HufJob *j, const char *t)
{
    pthread_mutex_lock(&j->lock);
    j->fase = t;
    pthread_mutex_unlock(&j->lock);
}

static void fase_conteo(HufJob *j, pthread_t *th)
{
    fase_texto(j, "Contando frecuencias");
    int n = lanzar_pool(j, th, j->n_hilos, hilo_contar);
    if (n == 0) { errno = EAGAIN; fallar(j, "no se pudo crear ningun hilo"); return; }

    for (;;) {
        emitir(j, 0);
        pthread_mutex_lock(&j->lock);
        int fin = (j->contados >= j->n_bloques) || cancelado_locked(j) || j->error;
        if (!fin) esperar_cv(j, &j->cv_listo);
        pthread_mutex_unlock(&j->lock);
        if (fin) break;
        revisar_senal(j);
    }
    revisar_senal(j);
    unir_pool(th, n);                            /* barrera: todas las tablas fusionadas */
}

/* Escritor ordenado + pool de procesamiento. */
static void fase_escritura(HufJob *j, pthread_t *th)
{
    fase_texto(j, j->modo == HUF_COMPRIMIR ? "Codificando bloques" : "Descomprimiendo bloques");

    pthread_mutex_lock(&j->lock);
    j->sig_asignar = 0;
    pthread_mutex_unlock(&j->lock);

    int n = lanzar_pool(j, th, j->n_hilos, hilo_proceso);
    if (n == 0) { errno = EAGAIN; fallar(j, "no se pudo crear ningun hilo"); return; }

    for (;;) {
        emitir(j, 0);
        pthread_mutex_lock(&j->lock);
        while (!cancelado_locked(j) && !j->error && j->sig_escribir < j->n_bloques &&
               !j->res[j->sig_escribir].listo)
            esperar_cv(j, &j->cv_listo);

        if (cancelado_locked(j) || j->error || j->sig_escribir >= j->n_bloques) {
            pthread_mutex_unlock(&j->lock);
            break;
        }
        BlockResult b = j->res[j->sig_escribir];
        j->res[j->sig_escribir].data = NULL;     /* el escritor es ahora dueno del buffer */
        j->sig_escribir++;
        pthread_cond_broadcast(&j->cv_trabajo);  /* la ventana avanzo */
        pthread_mutex_unlock(&j->lock);

        /* E/S fuera del mutex. */
        int rc = 0;
        if (j->modo == HUF_COMPRIMIR) {
            uint8_t h[HUF_BLK_HDR];
            put32(h, b.orig_len); put32(h + 4, b.size); put32(h + 8, b.crc);
            rc = escribir_todo(j->fd_out, h, sizeof(h));
            j->bytes_out += sizeof(h);
        }
        if (rc == 0) rc = escribir_todo(j->fd_out, b.data, b.size);
        j->bytes_out += b.size;
        free(b.data);
        if (rc < 0) { fallar(j, "escritura del archivo de salida"); break; }

        pthread_mutex_lock(&j->lock);
        j->escritos++;
        pthread_mutex_unlock(&j->lock);
    }
    revisar_senal(j);
    if (cancelado_o_error(j)) {
        pthread_mutex_lock(&j->lock);
        despertar_todos_locked(j);               /* workers dormidos en la ventana */
        pthread_mutex_unlock(&j->lock);
    }
    unir_pool(th, n);
}

static int escribir_cabecera(HufJob *j)
{
    uint8_t h[HUF_HDR_FIJO];
    memcpy(h, HUF_MAGIC, 4);
    put32(h + 4, j->block_size);
    put64(h + 8, j->orig_size);
    put32(h + 16, j->n_bloques);
    memcpy(h + 20, j->tabla.len, 256);
    j->bytes_out += sizeof(h);
    return escribir_todo(j->fd_out, h, sizeof(h));
}

/* Lee la cabecera .huf, valida y escanea las cabeceras de bloque (offsets). */
static int leer_cabecera_y_escanear(HufJob *j)
{
    uint8_t h[HUF_HDR_FIJO];
    if (leer_en(j->fd_in, h, sizeof(h), 0) < 0) { fallar(j, "no se pudo leer la cabecera"); return -1; }
    if (memcmp(h, HUF_MAGIC, 4) != 0) { errno = 0; fallar(j, "no es un archivo HUF1 valido"); return -1; }

    j->block_size = get32(h + 4);
    j->orig_size  = get64(h + 8);
    j->n_bloques  = get32(h + 16);
    if (j->block_size == 0 || j->block_size > BLOCK_SIZE_MAX ||
        (uint64_t)j->n_bloques != (j->orig_size + j->block_size - 1) / j->block_size) {
        errno = 0; fallar(j, "cabecera inconsistente"); return -1;
    }
    if (huf_construir_tabla(&j->tabla, h + 20) < 0) {
        errno = 0; fallar(j, "tabla de codigos invalida"); return -1;
    }

    if (j->n_bloques == 0) return 0;
    /* Cada bloque ocupa como minimo su cabecera: rechaza conteos absurdos ANTES de reservar. */
    if ((uint64_t)HUF_HDR_FIJO + (uint64_t)j->n_bloques * HUF_BLK_HDR > (uint64_t)j->st_ini.st_size) {
        errno = 0; fallar(j, "archivo truncado o conteo de bloques invalido"); return -1;
    }
    j->meta = (BlockMeta *)calloc(j->n_bloques, sizeof(BlockMeta));
    if (j->meta == NULL) { errno = ENOMEM; fallar(j, "memoria"); return -1; }

    uint64_t off = HUF_HDR_FIJO, total = 0;
    for (uint32_t i = 0; i < j->n_bloques; i++) {
        uint8_t bh[HUF_BLK_HDR];
        if ((i & 1023u) == 0 && cancelado_o_error(j)) return -1;
        if (leer_en(j->fd_in, bh, sizeof(bh), (off_t)off) < 0) {
            fallar(j, "archivo truncado en el bloque %u", i); return -1;
        }
        BlockMeta *m = &j->meta[i];
        m->off = off;
        m->orig_len = get32(bh); m->comp_len = get32(bh + 4); m->crc = get32(bh + 8);
        if (m->orig_len > j->block_size ||
            m->comp_len > ((uint64_t)m->orig_len * HUF_MAX_LEN + 7) / 8 + 8 ||
            off + HUF_BLK_HDR + m->comp_len > (uint64_t)j->st_ini.st_size) {
            errno = 0; fallar(j, "bloque %u con tamanos invalidos", i); return -1;
        }
        total += m->orig_len;
        off += HUF_BLK_HDR + m->comp_len;
    }
    if (total != j->orig_size) { errno = 0; fallar(j, "tamano total inconsistente"); return -1; }
    return 0;
}

static void *coordinador(void *arg)
{
    HufJob *j = (HufJob *)arg;
    pthread_t th[MAX_HILOS];
    HufEstado final;
    int renombrar = 0;

    pthread_mutex_lock(&j->lock);
    j->fase = "Iniciando";
    pthread_mutex_unlock(&j->lock);

    if (j->modo == HUF_COMPRIMIR) {
        /* n_bloques ya se conoce: se reserva res con su tamano definitivo. */
        j->res = (BlockResult *)calloc(j->n_bloques ? j->n_bloques : 1, sizeof(BlockResult));
        if (j->res == NULL) { errno = ENOMEM; fallar(j, "memoria"); goto fin; }
        if (j->n_bloques > 0) {
            fase_conteo(j, th);
            if (cancelado_o_error(j)) goto fin;
        }
        uint8_t len[256];
        if (huf_longitudes(j->freq_global, len) < 0) {
            errno = 0; fallar(j, "arbol de Huffman demasiado profundo"); goto fin;
        }
        if (huf_construir_tabla(&j->tabla, len) < 0) {
            errno = 0; fallar(j, "no se pudo construir la tabla"); goto fin;
        }
        if (escribir_cabecera(j) < 0) { fallar(j, "escritura de la cabecera"); goto fin; }
    } else {
        fase_texto(j, "Leyendo cabeceras");
        if (leer_cabecera_y_escanear(j) < 0) goto fin;
        /* La tabla ya quedo construida. Ahora SI se conoce n_bloques (validado). */
        j->res = (BlockResult *)calloc(j->n_bloques ? j->n_bloques : 1, sizeof(BlockResult));
        if (j->res == NULL) { errno = ENOMEM; fallar(j, "memoria"); goto fin; }
    }

    if (j->n_bloques > 0) {
        fase_escritura(j, th);
        if (cancelado_o_error(j)) goto fin;
    }

    /* El origen no debe haber cambiado mientras lo leiamos. */
    {
        struct stat st;
        if (fstat(j->fd_in, &st) < 0) { fallar(j, "fstat del origen"); goto fin; }
        if (st.st_size != j->st_ini.st_size || st.st_mtim.tv_sec != j->st_ini.st_mtim.tv_sec ||
            st.st_mtim.tv_nsec != j->st_ini.st_mtim.tv_nsec) {
            errno = 0; fallar(j, "el archivo de origen fue modificado durante la operacion"); goto fin;
        }
    }
    if (fsync(j->fd_out) < 0) { fallar(j, "fsync"); goto fin; }
    renombrar = 1;

fin:
    {
        int hubo_error, hubo_cancel;
        pthread_mutex_lock(&j->lock);
        hubo_error = j->error;
        hubo_cancel = cancelado_locked(j);
        pthread_mutex_unlock(&j->lock);

        if (j->fd_out >= 0) {
            if (close(j->fd_out) < 0 && !hubo_error && renombrar) { fallar(j, "close del destino"); hubo_error = 1; }
            j->fd_out = -1;
        }
        if (renombrar && !hubo_error && !hubo_cancel) {
            if (rename(j->ruta_tmp, j->ruta_out) < 0) { fallar(j, "rename al destino final"); hubo_error = 1; }
        }
        if (hubo_error)        final = HUF_ERROR;
        else if (hubo_cancel)  final = HUF_CANCELADO;
        else if (!renombrar)   final = HUF_ERROR;
        else                   final = HUF_OK;
        if (final != HUF_OK) unlink(j->ruta_tmp);   /* nunca dejar un archivo a medias */

        if (j->fd_in >= 0) { close(j->fd_in); j->fd_in = -1; }
        if (j->res != NULL) {
            for (uint32_t i = 0; i < j->n_bloques; i++) free(j->res[i].data);
            free(j->res); j->res = NULL;
        }
        free(j->meta); j->meta = NULL;

        pthread_mutex_lock(&j->lock);
        if (final == HUF_OK) {
            if (j->modo == HUF_COMPRIMIR)
                snprintf(j->msg, sizeof(j->msg), "Comprimido: %llu -> %llu bytes (%.1f%%)",
                         (unsigned long long)j->orig_size, (unsigned long long)j->bytes_out,
                         j->orig_size ? 100.0 * (double)j->bytes_out / (double)j->orig_size : 100.0);
            else
                snprintf(j->msg, sizeof(j->msg), "Descomprimido: %llu bytes escritos en '%.200s'",
                         (unsigned long long)j->orig_size, j->ruta_out);
        } else if (final == HUF_CANCELADO) {
            snprintf(j->msg, sizeof(j->msg), "Operacion cancelada; no se dejo ningun archivo parcial");
        }
        j->estado = final;
        pthread_mutex_unlock(&j->lock);
    }

    emitir(j, 1);

    pthread_mutex_lock(&j->lock);
    j->terminado = 1;
    pthread_cond_broadcast(&j->cv_fin);
    pthread_mutex_unlock(&j->lock);
    return NULL;
}

/* ------------------------------ API publica ------------------------------------- */

HufJob *huf_job_iniciar(HufModo modo, const char *entrada, const char *salida,
                        int hilos, HufProgresoCb cb, void *ud)
{
    HufJob *j = (HufJob *)calloc(1, sizeof(HufJob));
    if (j == NULL) { fprintf(stderr, "huf: sin memoria\n"); return NULL; }

    j->modo = modo; j->cb = cb; j->ud = ud;
    j->fd_in = j->fd_out = -1;
    snprintf(j->ruta_in, sizeof(j->ruta_in), "%s", entrada);
    snprintf(j->ruta_out, sizeof(j->ruta_out), "%s", salida);
    snprintf(j->ruta_tmp, sizeof(j->ruta_tmp), "%s.huftmp", salida);

    if (hilos <= 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        hilos = (c < 2) ? 2 : (c > 8 ? 8 : (int)c);
    }
    if (hilos > MAX_HILOS) hilos = MAX_HILOS;
    j->n_hilos = hilos;
    j->ventana = (uint32_t)(4 * hilos < 8 ? 8 : 4 * hilos);
    j->block_size = HUF_BLOCK_SIZE;

    j->fd_in = open(entrada, O_RDONLY);
    if (j->fd_in < 0) { fprintf(stderr, "huf: open('%s'): %s\n", entrada, strerror(errno)); goto fallo; }
    if (fstat(j->fd_in, &j->st_ini) < 0) { fprintf(stderr, "huf: fstat: %s\n", strerror(errno)); goto fallo; }
    if (!S_ISREG(j->st_ini.st_mode)) { fprintf(stderr, "huf: '%s' no es un archivo regular\n", entrada); goto fallo; }
    j->in_dev = j->st_ini.st_dev; j->in_ino = j->st_ini.st_ino;

    {
        struct stat so;
        if (stat(salida, &so) == 0) {
            if (so.st_dev == j->in_dev && so.st_ino == j->in_ino) {
                fprintf(stderr, "huf: origen y destino son el mismo archivo\n");
                goto fallo;
            }
            j->out_previo = 1; j->out_dev = so.st_dev; j->out_ino = so.st_ino;
        }
    }

    if (modo == HUF_COMPRIMIR) {
        j->orig_size = (uint64_t)j->st_ini.st_size;
        j->n_bloques = (uint32_t)((j->orig_size + j->block_size - 1) / j->block_size);
        if ((uint64_t)j->n_bloques * j->block_size < j->orig_size) {   /* desborde u32 */
            fprintf(stderr, "huf: archivo demasiado grande\n"); goto fallo;
        }
    }

    j->fd_out = open(j->ruta_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (j->fd_out < 0) { fprintf(stderr, "huf: open('%s'): %s\n", j->ruta_tmp, strerror(errno)); goto fallo; }

    {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_mutex_init(&j->lock, NULL);
        pthread_cond_init(&j->cv_trabajo, &ca);
        pthread_cond_init(&j->cv_listo, &ca);
        pthread_cond_init(&j->cv_fin, &ca);
        pthread_condattr_destroy(&ca);
    }

    atomic_store(&g_abort, 0);
    j->estado = HUF_CORRIENDO;          /* ANTES de crear el hilo: el candado del editor ya rige */
    j->pct_reportado = -1;
    j->coord_vivo = 1;
    if (pthread_create(&j->hilo_coord, NULL, coordinador, j) != 0) {
        fprintf(stderr, "huf: no se pudo crear el hilo coordinador\n");
        j->coord_vivo = 0;
        unlink(j->ruta_tmp);
        pthread_mutex_destroy(&j->lock);
        pthread_cond_destroy(&j->cv_trabajo);
        pthread_cond_destroy(&j->cv_listo);
        pthread_cond_destroy(&j->cv_fin);
        goto fallo;
    }
    return j;

fallo:
    if (j->fd_in >= 0) close(j->fd_in);
    if (j->fd_out >= 0) { close(j->fd_out); unlink(j->ruta_tmp); }
    free(j);
    return NULL;
}

HufEstado huf_job_estado(HufJob *j, int *porcentaje, char *msg, size_t msg_cap)
{
    pthread_mutex_lock(&j->lock);
    HufEstado e = j->estado;
    if (porcentaje) *porcentaje = pct_locked(j);
    if (msg && msg_cap) {
        if (e == HUF_CORRIENDO) snprintf(msg, msg_cap, "%s", j->fase ? j->fase : "");
        else                    snprintf(msg, msg_cap, "%s", j->msg);
    }
    pthread_mutex_unlock(&j->lock);
    return e;
}

void huf_job_cancelar(HufJob *j)
{
    pthread_mutex_lock(&j->lock);
    j->cancelado = 1;
    despertar_todos_locked(j);
    pthread_mutex_unlock(&j->lock);
}

static void unir_coordinador(HufJob *j)
{
    pthread_mutex_lock(&j->lock);
    int debo = j->coord_vivo;
    j->coord_vivo = 0;                   /* solo un hilo hace el join */
    pthread_mutex_unlock(&j->lock);
    if (debo) pthread_join(j->hilo_coord, NULL);
}

HufEstado huf_job_esperar(HufJob *j)
{
    pthread_mutex_lock(&j->lock);
    while (!j->terminado) pthread_cond_wait(&j->cv_fin, &j->lock);
    HufEstado e = j->estado;
    pthread_mutex_unlock(&j->lock);
    unir_coordinador(j);
    return e;
}

void huf_job_liberar(HufJob *j)
{
    if (j == NULL) return;
    huf_job_cancelar(j);
    huf_job_esperar(j);
    pthread_mutex_destroy(&j->lock);
    pthread_cond_destroy(&j->cv_trabajo);
    pthread_cond_destroy(&j->cv_listo);
    pthread_cond_destroy(&j->cv_fin);
    free(j);
}

int huf_job_bloquea_fd(HufJob *j, int fd)
{
    if (j == NULL) return 0;
    pthread_mutex_lock(&j->lock);
    int corriendo = (j->estado == HUF_CORRIENDO);
    pthread_mutex_unlock(&j->lock);
    if (!corriendo) return 0;

    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    if (st.st_dev == j->in_dev && st.st_ino == j->in_ino) return 1;
    if (j->out_previo && st.st_dev == j->out_dev && st.st_ino == j->out_ino) return 1;
    return 0;
}
