#ifndef HUF_H
#define HUF_H

/**
 * ====================================================================================
 *  huf.h  -  COMPRESOR HUFFMAN CONCURRENTE  (SO2026B - Parcial 2, Alternativa 1)
 * ====================================================================================
 *  Dos capas:
 *    huf_core.c  -> CODEC secuencial puro (sin hilos): longitudes de codigo, codigos
 *                   canonicos, CRC32, codificar/decodificar UN bloque.
 *    huf_job.c   -> PIPELINE concurrente: pool de hilos, cola de trabajo, escritor
 *                   ordenado, progreso y cancelacion.
 *
 *  Formato del archivo comprimido (enteros en little-endian):
 *    "HUF1" | block_size u32 | orig_size u64 | n_bloques u32 | len[256] (1 byte c/u)
 *    por cada bloque:  orig_len u32 | comp_len u32 | crc32 u32 | bits[comp_len]
 * ====================================================================================
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define HUF_MAGIC        "HUF1"
#define HUF_BLOCK_SIZE   (64u * 1024u)    /* bytes de entrada por bloque            */
#define HUF_MAX_LEN      56               /* longitud maxima de un codigo (bits)    */
#define HUF_HDR_FIJO     (4 + 4 + 8 + 4 + 256)
#define HUF_BLK_HDR      12

/* ------------------------------ CODEC (huf_core.c) ------------------------------ */

typedef struct {
    uint8_t  len[256];        /* longitud del codigo de cada simbolo (0 = no aparece) */
    uint64_t code[256];       /* codigo canonico alineado a la derecha                */
    int      max_len;
    /* Tablas de decodificacion canonica, indexadas por longitud */
    uint64_t first_code[HUF_MAX_LEN + 2];
    uint32_t count[HUF_MAX_LEN + 2];
    uint32_t first_idx[HUF_MAX_LEN + 2];
    uint8_t  sorted[256];     /* simbolos ordenados por (longitud, valor)             */
    int      n_simbolos;
} HufTabla;

uint32_t huf_crc32(const uint8_t *buf, size_t n);
/* Calcula longitudes optimas a partir de frecuencias. 0 = ok, -1 = codigo demasiado largo. */
int      huf_longitudes(const uint64_t freq[256], uint8_t len[256]);
/* Construye codigos canonicos y tablas de decodificacion desde len[]. 0 = ok, -1 = invalido. */
int      huf_construir_tabla(HufTabla *t, const uint8_t len[256]);
/* Codifica un bloque. Devuelve malloc() con los bits, y su tamano en *out_len. NULL si falla. */
uint8_t *huf_codificar_bloque(const HufTabla *t, const uint8_t *in, size_t n, size_t *out_len);
/* Decodifica un bloque de orig_len simbolos. 0 = ok, -1 = datos corruptos. */
int      huf_decodificar_bloque(const HufTabla *t, const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t orig_len);

/* ------------------------------ PIPELINE (huf_job.c) ---------------------------- */

typedef enum {
    HUF_IDLE = 0,
    HUF_CORRIENDO,
    HUF_OK,
    HUF_ERROR,
    HUF_CANCELADO
} HufEstado;

typedef enum { HUF_COMPRIMIR = 0, HUF_DESCOMPRIMIR = 1 } HufModo;

struct HufJob;
typedef struct HufJob HufJob;

/* Se invoca desde el hilo coordinador SIN ningun mutex del job tomado, solo cuando
   cambia el porcentaje. 'estado' indica si sigue corriendo o ya termino. */
typedef void (*HufProgresoCb)(void *ud, int porcentaje, HufEstado estado, const char *mensaje);

/* Crea y lanza el trabajo en segundo plano. Retorna NULL si no pudo iniciar (errno/mensaje
   por stderr). 'hilos' <= 0 usa el numero de CPUs en linea. */
HufJob   *huf_job_iniciar(HufModo modo, const char *entrada, const char *salida,
                          int hilos, HufProgresoCb cb, void *ud);
/* Foto instantanea del estado (segura desde cualquier hilo). */
HufEstado huf_job_estado(HufJob *j, int *porcentaje, char *msg, size_t msg_cap);
/* Pide cancelar (no bloquea). */
void      huf_job_cancelar(HufJob *j);
/* Espera (variable de condicion, sin espera activa) a que termine el trabajo. */
HufEstado huf_job_esperar(HufJob *j);
/* Libera el job (hace join del coordinador si aun no se hizo). Cancela si seguia vivo. */
void      huf_job_liberar(HufJob *j);
/* 1 si el job esta corriendo Y el descriptor fd apunta a uno de los archivos que usa
   (origen o destino previo): en ese caso el editor NO debe escribir en el. */
int       huf_job_bloquea_fd(HufJob *j, int fd);
/* Marca de abortar global (para manejadores de senal). Async-signal-safe. */
void      huf_abortar_por_senal(void);

#endif /* HUF_H */
