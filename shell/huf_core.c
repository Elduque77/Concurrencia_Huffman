/**
 * ====================================================================================
 *  huf_core.c  -  CODEC HUFFMAN SECUENCIAL (sin hilos)
 * ====================================================================================
 *  Esta capa NO conoce hilos ni archivos: solo transforma bytes <-> bits. Eso permite
 *  probarla de forma aislada y que el pipeline concurrente (huf_job.c) la use desde
 *  varios hilos a la vez sin sincronizacion, porque:
 *    - HufTabla es de SOLO LECTURA una vez construida.
 *    - Cada llamada trabaja sobre buffers propios del llamador.
 *
 *  Se usan codigos CANONICOS: el archivo solo guarda la longitud de cada codigo (256
 *  bytes) y el decodificador reconstruye exactamente los mismos codigos.
 * ====================================================================================
 */

#include "huf.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ----------------------------------- CRC32 ------------------------------------- */

static uint32_t       crc_tabla[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tabla[i] = c;
    }
}

uint32_t huf_crc32(const uint8_t *buf, size_t n)
{
    pthread_once(&crc_once, crc_init);   /* seguro aunque varios hilos entren a la vez */
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_tabla[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------- Longitudes de codigo (Huffman) ----------------------- */

int huf_longitudes(const uint64_t freq[256], uint8_t len[256])
{
    uint64_t nf[511];
    int      padre[511];
    int      vivo[511];
    int      n_nodos = 0, presentes = 0;

    memset(len, 0, 256);
    for (int s = 0; s < 256; s++) {
        nf[s] = freq[s];
        padre[s] = -1;
        vivo[s] = (freq[s] > 0);
        if (freq[s] > 0) presentes++;
    }
    n_nodos = 256;

    if (presentes == 0) return 0;
    if (presentes == 1) {                 /* Caso borde: un solo simbolo -> codigo "0" */
        for (int s = 0; s < 256; s++) if (freq[s] > 0) len[s] = 1;
        return 0;
    }

    /* Se fusionan los dos nodos vivos de menor frecuencia (empate: menor indice), lo
       que hace el resultado DETERMINISTA. n <= 256 -> O(n^2) es despreciable. */
    for (int paso = 0; paso < presentes - 1; paso++) {
        int a = -1, b = -1;
        for (int i = 0; i < n_nodos; i++) {
            if (!vivo[i]) continue;
            if (a < 0 || nf[i] < nf[a])      { b = a; a = i; }
            else if (b < 0 || nf[i] < nf[b]) { b = i; }
        }
        nf[n_nodos]    = nf[a] + nf[b];
        padre[n_nodos] = -1;
        vivo[n_nodos]  = 1;
        padre[a] = padre[b] = n_nodos;
        vivo[a] = vivo[b] = 0;
        n_nodos++;
    }

    for (int s = 0; s < 256; s++) {
        if (freq[s] == 0) continue;
        int prof = 0;
        for (int x = s; padre[x] >= 0; x = padre[x]) prof++;
        if (prof > HUF_MAX_LEN) return -1;
        len[s] = (uint8_t)prof;
    }
    return 0;
}

/* ------------------------------ Tablas canonicas -------------------------------- */

int huf_construir_tabla(HufTabla *t, const uint8_t len[256])
{
    memset(t, 0, sizeof(*t));
    memcpy(t->len, len, 256);

    uint64_t kraft = 0;                         /* suma de 2^(MAX-len) */
    for (int s = 0; s < 256; s++) {
        if (len[s] == 0) continue;
        if (len[s] > HUF_MAX_LEN) return -1;
        t->count[len[s]]++;
        t->n_simbolos++;
        if (len[s] > t->max_len) t->max_len = len[s];
        kraft += (uint64_t)1 << (HUF_MAX_LEN - len[s]);
    }
    if (kraft > ((uint64_t)1 << HUF_MAX_LEN)) return -1;   /* tabla imposible/corrupta */

    uint64_t code = 0;
    uint32_t idx = 0;
    for (int l = 1; l <= t->max_len; l++) {
        t->first_code[l] = code;
        t->first_idx[l]  = idx;
        idx  += t->count[l];
        code  = (code + t->count[l]) << 1;
    }

    uint32_t pos = 0;
    for (int l = 1; l <= t->max_len; l++) {
        uint32_t k = 0;
        for (int s = 0; s < 256; s++) {
            if (len[s] != l) continue;
            t->sorted[pos++] = (uint8_t)s;
            t->code[s] = t->first_code[l] + k++;
        }
    }
    return 0;
}

/* ------------------------------ Bloque: codificar ------------------------------- */

uint8_t *huf_codificar_bloque(const HufTabla *t, const uint8_t *in, size_t n, size_t *out_len)
{
    uint64_t bits = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t l = t->len[in[i]];
        if (l == 0) return NULL;                 /* simbolo fuera de la tabla */
        bits += l;
    }
    size_t total = (size_t)((bits + 7) / 8);
    uint8_t *out = (uint8_t *)malloc(total ? total : 1);
    if (out == NULL) return NULL;

    uint64_t acc = 0;
    int      nb = 0;
    size_t   o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t s = in[i];
        acc = (acc << t->len[s]) | t->code[s];
        nb += t->len[s];
        while (nb >= 8) {
            out[o++] = (uint8_t)((acc >> (nb - 8)) & 0xFF);
            nb -= 8;
        }
    }
    if (nb > 0) out[o++] = (uint8_t)((acc << (8 - nb)) & 0xFF);

    *out_len = total;
    return out;
}

/* ------------------------------ Bloque: decodificar ----------------------------- */

int huf_decodificar_bloque(const HufTabla *t, const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t orig_len)
{
    if (orig_len == 0) return 0;
    if (t->max_len == 0) return -1;

    uint64_t bitpos = 0;
    uint64_t limite = (uint64_t)in_len * 8;

    for (size_t i = 0; i < orig_len; i++) {
        uint64_t code = 0;
        int      l, ok = 0;
        for (l = 1; l <= t->max_len; l++) {
            if (bitpos >= limite) return -1;     /* se acabaron los bits: corrupto */
            uint64_t bit = (in[bitpos >> 3] >> (7 - (bitpos & 7))) & 1u;
            bitpos++;
            code = (code << 1) | bit;
            if (t->count[l] && code >= t->first_code[l] &&
                code - t->first_code[l] < t->count[l]) {
                out[i] = t->sorted[t->first_idx[l] + (uint32_t)(code - t->first_code[l])];
                ok = 1;
                break;
            }
        }
        if (!ok) return -1;
    }
    return 0;
}
