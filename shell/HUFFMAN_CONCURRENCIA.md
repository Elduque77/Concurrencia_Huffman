# Compresor Huffman concurrente integrado al editor (Parcial 2 - Alternativa 1)

## Uso (dentro del editor `edi` o del shell `eafitOS` → `edi`)

| Comando | Acción |
|---|---|
| `k [salida]` | Comprime el archivo abierto en segundo plano (por defecto `<archivo>.huf`) |
| `u <x.huf> [salida]` | Descomprime en segundo plano (por defecto quita `.huf`; no pisa archivos existentes) |
| `j` | Muestra el progreso en tiempo real |
| `n` | Cancela la operación (también `Ctrl+C` mientras corre) |

Los avisos de progreso (`[huf] [#####...] 50%`) aparecen cada 10 % sin congelar el prompt.
Herramienta independiente: `./huf c|d <entrada> <salida> [-t hilos]`.

## Archivos

| Archivo | Rol |
|---|---|
| `huf_core.c` | Códec **secuencial**: longitudes de Huffman, códigos canónicos, CRC32, codificar/decodificar un bloque |
| `huf_job.c` | **Pipeline concurrente**: hilo coordinador, pool de workers, ventana acotada, escritor ordenado, progreso, cancelación |
| `editor_huf.c` | Integración: comandos `k u j n`, candado de escritura, prompt re-dibujable, Ctrl+C |
| `huf_main.c` | Binario `huf` para pruebas con `md5sum`, TSan, etc. |

## Hilos y fases

```
REPL (hilo principal) ── k ──► Coordinador (1 hilo)
                                 ├─ Fase 1  N workers cuentan frecuencias (tabla LOCAL c/u) ─ join
                                 ├─ Fase 2  construye el árbol (secuencial, 256 símbolos) y escribe la cabecera
                                 └─ Fase 3  N workers codifican bloques ──► results[idx]
                                            el COORDINADOR es el escritor: escribe results[0], [1], [2]… en orden
```

Descompresión: el coordinador escanea las cabeceras de bloque (offsets) y la fase 3 decodifica y verifica el CRC32 de cada bloque.

## Mecanismos de sincronización

Un único mutex `lock` protege todo el estado mutable del job. Tres variables de condición:

| Variable | Quién espera | Qué espera |
|---|---|---|
| `cv_trabajo` | workers | que la **ventana** avance (no adelantarse más de `4·N` bloques al escritor → memoria acotada) |
| `cv_listo` | coordinador/escritor | que `results[sig_escribir]` esté listo, avance el conteo o haya error/cancelación |
| `cv_fin` | `huf_job_esperar` | que termine el job |

* **Reducción local (fase 1):** cada worker cuenta en su propio `freq[256]` y fusiona con **un solo lock** al terminar; no hay contención por byte.
* **Orden de salida:** los bloques terminan desordenados, pero el escritor solo avanza cuando `results[sig_escribir].listo` es verdadero.
* **Lectura paralela segura:** `pread(2)` no mueve el cursor compartido, así que los workers leen el mismo `fd` sin lock.

### Por qué no hay problemas de concurrencia

* **Sin espera activa:** toda espera es `pthread_cond_wait`/`timedwait` (duerme). El `timedwait` de 200 ms existe solo para observar la bandera de señal, porque `pthread_cond_broadcast` no es *async-signal-safe*.
* **Sin interbloqueo:** hay un solo mutex (no existe orden de locks que violar); nunca se hace E/S, `malloc` de bloques ni callback con el mutex tomado. El bloque `sig_escribir` siempre está asignado a un worker que lo termina sin esperar a nadie, así que la ventana nunca puede bloquear el progreso.
* **Sin condiciones de carrera:** todo campo mutable se toca bajo `lock`; el resto (tabla de códigos, metadatos) se escribe antes de `pthread_create` y luego es de solo lectura. Verificado con **ThreadSanitizer: 0 avisos** (códec, pipeline y editor completo).

## Condiciones de carrera con la edición

Cada edición del editor se persiste de inmediato sobre el mismo inodo. Mientras un job corre:

* **Lecturas** (`p s y m c`): permitidas (lectura compartida).
* **Escrituras** (`a i d x`): `ed_huf_puede_escribir()` compara `(st_dev, st_ino)` del fd abierto con el origen/destino del job (bajo su mutex) y las **rechaza** con un mensaje.
* **Sin TOCTOU:** solo el hilo del REPL lanza jobs y ese hilo no está editando en ese momento; además el job pasa a `CORRIENDO` **antes** de crear su hilo.
* **Defensa extra:** al terminar se compara tamaño y mtime del origen; si cambió (modificación externa) aborta sin dejar `.huf` corrupto.

## Formato del `.huf` (little-endian)

```
"HUF1" | block_size u32 | orig_size u64 | n_bloques u32 | len[256] (1 byte c/u)
por bloque: orig_len u32 | comp_len u32 | crc32 u32 | bits[comp_len]
```
Códigos **canónicos**: solo se guardan las longitudes (256 bytes). La salida es **determinista**: idéntica con 1 o con N hilos.

## Manejo de errores y recursos

* Salida **atómica**: se escribe `<salida>.huftmp`, `fsync` y `rename`. Error o cancelación ⇒ se borra el temporal; nunca queda un archivo a medias.
* Siempre `join` de todos los hilos, `free` de buffers/resultados pendientes y `close` de ambos descriptores (verificado con ASan/UBSan/LSan: 0 errores, 0 fugas).
* `q` con un job activo lo cancela y espera antes de salir. `Ctrl+C` solo se intercepta mientras hay un job; luego se restaura el comportamiento original.
* Entradas hostiles: magic inválido, truncado, bytes alterados, tamaños absurdos y tabla imposible dan error controlado (probado con fuzz simple bajo ASan).

## Pruebas

```
make pruebas_huf                      # códec + pipeline: 28 verificaciones (cmp/md5, determinismo, corrupción, cancelación, TSan)
python3 pruebas_huf_editor.py [MB]    # integración con el editor real: 12 verificaciones
```

## Mapa a la rúbrica

| Criterio | Evidencia |
|---|---|
| Concurrencia en Huffman (25) | Conteo paralelo con reducción local, codificación paralela por bloques, escritor ordenado |
| Sincronización (25) | mutex + 3 cond. vars, ventana acotada, 0 avisos TSan, sin espera activa, argumento de no-deadlock |
| Integración con el editor (20) | Segundo plano, barra de progreso, candado de escritura, prompt re-dibujable, cancelación |
| Corrección algorítmica (15) | `cmp`/`md5` idénticos en 8 tipos de archivo × 3 conteos de hilos + CRC32 por bloque |
| Errores y recursos (15) | Salida atómica, join/free/close verificados, señales, fuzz |

## Limitaciones conocidas

* La decodificación es bit a bit (clara y correcta, no la más rápida); una tabla de búsqueda aceleraría la descompresión.
* Rutas con espacios no se soportan en `u` (el parser usa dos palabras).
* Un solo trabajo de compresión a la vez por sesión del editor.
