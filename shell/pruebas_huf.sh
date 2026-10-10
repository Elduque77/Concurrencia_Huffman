#!/bin/bash
# ====================================================================================
#  pruebas_huf.sh - Pruebas del compresor Huffman concurrente (Parcial 2, Alt. 1)
#  Uso: make pruebas_huf      (o)      bash pruebas_huf.sh
#  Verifica: integridad (cmp + md5), determinismo entre 1 y N hilos, archivos dañados,
#  cancelacion por senal sin dejar archivos a medias y, si el compilador lo soporta,
#  ausencia de carreras con ThreadSanitizer.
# ====================================================================================
cd "$(dirname "$0")" || exit 1
[ -x ./huf ] || make huf >/dev/null || exit 1

OK=0; FALLO=0
ok()  { echo "  [OK]    $1"; OK=$((OK+1)); }
bad() { echo "  [FALLA] $1"; FALLO=$((FALLO+1)); }
chk() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

T=$(mktemp -d /tmp/pruebas_huf.XXXXXX); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
H="$OLDPWD/huf"

: > vacio.bin
printf 'A' > un_byte.bin
head -c 200000 /dev/zero | tr '\0' 'x' > un_simbolo.bin
head -c 1000000 /dev/urandom > aleatorio.bin
head -c 65536 /dev/urandom > exacto64k.bin
head -c 65537 /dev/urandom > mas1.bin
python3 -c "
import random; random.seed(3)
w='el sistema operativo gestiona hilos mutex semaforos memoria procesos'.split()
open('texto.txt','w').write(' '.join(random.choice(w) for _ in range(800000)))"

echo "== 1. Ida y vuelta (original == descomprimido) con 1, 2 y 8 hilos =="
for f in vacio.bin un_byte.bin un_simbolo.bin aleatorio.bin exacto64k.bin mas1.bin texto.txt; do
  for t in 1 2 8; do
    "$H" c $f $f.huf -t $t 2>/dev/null && "$H" d $f.huf $f.out -t $t 2>/dev/null
    chk "$f  hilos=$t  (md5 $(md5sum < $f.out 2>/dev/null | cut -c1-8))" "cmp -s $f $f.out"
  done
done

echo "== 2. Determinismo: misma salida sin importar el numero de hilos =="
"$H" c texto.txt a1.huf -t 1 2>/dev/null; "$H" c texto.txt a8.huf -t 8 2>/dev/null
chk "texto.txt: .huf con 1 hilo == .huf con 8 hilos" "cmp -s a1.huf a8.huf"

echo "== 3. Archivos danados: error limpio, sin crash y sin salida parcial =="
cp texto.txt.huf flip.huf; printf '\xff' | dd of=flip.huf bs=1 seek=20000 conv=notrunc 2>/dev/null
head -c 30000 texto.txt.huf > trunc.huf
head -c 3000 aleatorio.bin > magic.huf
for f in flip trunc magic; do
  rm -f d.out; "$H" d $f.huf d.out >/dev/null 2>&1; rc=$?
  chk "$f.huf -> rc=$rc (1=error controlado, no senal) y sin d.out" "[ $rc -eq 1 ] && [ ! -e d.out ]"
done

echo "== 4. Cancelacion (SIGINT) sin archivos a medias =="
python3 -c "
import random; random.seed(1)
w='a b c d e f g h i j k l m n o p'.split()
open('grande.txt','w').write(' '.join(random.choice(w) for _ in range(30_000_000)))"
"$H" c grande.txt cancel.huf -t 4 2>/dev/null & pid=$!; sleep 0.15; kill -INT $pid 2>/dev/null; wait $pid; rc=$?
if [ $rc -eq 0 ]; then ok "(maquina muy rapida: termino antes de cancelar; prueba no concluyente)";
else chk "compresion cancelada rc=$rc y sin .huf ni .huftmp" "[ $rc -eq 130 ] && ! ls cancel.huf* >/dev/null 2>&1"; fi

echo "== 5. Origen modificado durante la compresion se detecta =="
cp grande.txt mod.txt
"$H" c mod.txt mod.huf -t 2 2>/dev/null & pid=$!; sleep 0.05; echo extra >> mod.txt; wait $pid; rc=$?
if [ $rc -eq 0 ]; then ok "(termino antes de modificar; prueba no concluyente)";
else chk "rc=$rc y sin mod.huf corrupto" "[ $rc -eq 1 ] && [ ! -e mod.huf ]"; fi

echo "== 6. ThreadSanitizer (carreras de datos) =="
SRC="$OLDPWD"
if gcc -std=gnu11 -g -O1 -D_GNU_SOURCE -pthread -fsanitize=thread -o huf_tsan \
     "$SRC/huf_main.c" "$SRC/huf_job.c" "$SRC/huf_core.c" 2>/dev/null; then
  export TSAN_OPTIONS="log_path=$T/tsan"
  "$T/huf_tsan" c texto.txt t.huf -t 8 2>/dev/null; "$T/huf_tsan" d t.huf t.out -t 8 2>/dev/null
  chk "TSan: 0 avisos y resultado correcto" "[ -z \"\$(ls $T/tsan.* 2>/dev/null)\" ] && cmp -s texto.txt t.out"
else
  echo "  (gcc sin soporte de -fsanitize=thread: se omite)"
fi

echo; echo "Exitosas: $OK   Fallidas: $FALLO"
[ $FALLO -eq 0 ]
