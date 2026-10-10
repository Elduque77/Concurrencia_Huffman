#!/usr/bin/env python3
"""
pruebas_huf_editor.py - Prueba de INTEGRACION del compresor con el editor 'edi'.
Maneja el editor real por stdin/stdout, como un usuario, y verifica:
  compresion en segundo plano (el prompt responde), bloqueo de escrituras sobre el archivo
  en uso, lecturas permitidas, progreso, cancelacion, no pisar destinos, integridad (md5)
  y salida limpia con un trabajo activo.
Uso:  python3 pruebas_huf_editor.py [MB]     (MB = tamano del archivo de prueba, def. 300)
En maquinas muy rapidas sube MB para que la compresion dure lo suficiente.
"""
import subprocess, threading, time, re, hashlib, os, sys, tempfile, random

AQUI = os.path.dirname(os.path.abspath(__file__))
EDI = os.path.join(AQUI, 'edi')
MB = int(sys.argv[1]) if len(sys.argv) > 1 else 300
W = tempfile.mkdtemp(prefix='huf_ed_')
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]|\r')
res = []

def check(desc, cond):
    res.append(bool(cond)); print(('  [OK]    ' if cond else '  [FALLA] ') + desc)

def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()

def huerfanos(): return [f for f in os.listdir(W) if f.endswith('.huftmp')]

class Ed:
    def __init__(s):
        s.p = subprocess.Popen([EDI], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0, cwd=W)
        s.buf = ''; s.lock = threading.Lock()
        threading.Thread(target=s._rd, daemon=True).start()
    def _rd(s):
        while True:
            b = os.read(s.p.stdout.fileno(), 65536)
            if not b: break
            with s.lock: s.buf += ANSI.sub('', b.decode('utf8', 'replace'))
    def send(s, l): s.p.stdin.write((l + '\n').encode()); s.p.stdin.flush()
    def mark(s):
        with s.lock: return len(s.buf)
    def since(s, m):
        with s.lock: return s.buf[m:]
    def wait(s, pat, m, t=60):
        t0 = time.time()
        while time.time() - t0 < t:
            if re.search(pat, s.since(m)): return True
            time.sleep(0.02)
        return False

print(f'Generando archivo de prueba de ~{MB} MB en {W} ...')
random.seed(1); w = 'el compilador genera codigo para el sistema operativo y los hilos comparten memoria'.split()
chunk = ' '.join(random.choice(w) for _ in range(500000)) + '\n'
with open(os.path.join(W, 'doc.txt'), 'w') as f:
    for _ in range(max(1, MB * 1024 * 1024 // len(chunk))): f.write(chunk)
orig = md5(os.path.join(W, 'doc.txt')); orig_size = os.path.getsize(os.path.join(W, 'doc.txt'))

e = Ed(); time.sleep(0.2); e.send('t'); time.sleep(0.1)       # sin trazado de syscalls
m = e.mark(); e.send('o doc.txt'); check('abre el archivo', e.wait(r'abierto', m))

print('-- compresion en segundo plano --')
m = e.mark(); e.send('k'); e.wait(r'Comprimiendo', m); e.send('m')
check('k vuelve al prompt de inmediato y el editor sigue respondiendo (lectura "m")',
      e.wait(r'[Tt]amano|bytes', m, 5))
m2 = e.mark(); e.send('a LINEA_QUE_NO_DEBE_ENTRAR')
corriendo = not re.search(r'Comprimido:', e.since(m))
if corriendo:
    check('escritura "a" rechazada mientras comprime (archivo bloqueado)', e.wait(r'Archivo bloqueado', m2, 5))
else:
    print('  [----]  la compresion termino antes de intentar escribir: sube MB (no concluyente)')
m3 = e.mark(); e.send('j'); e.wait(r'\[huf\]|No hay', m3, 5)
check('j informa el progreso', 'huf' in e.since(m3) or 'No hay' in e.since(m3))
check('compresion finaliza con el resumen', e.wait(r'Comprimido: \d+ -> \d+ bytes', m))
check('el archivo original quedo intacto (md5 y tamano)',
      md5(os.path.join(W, 'doc.txt')) == orig and os.path.getsize(os.path.join(W, 'doc.txt')) == orig_size)
check('no quedan archivos temporales .huftmp', not huerfanos())
m = e.mark(); e.send('a linea nueva'); check('terminada la tarea, la escritura vuelve a permitirse', e.wait(r'anexada', m, 20))

print('-- descompresion e integridad --')
m = e.mark(); e.send('u doc.txt.huf copia.txt'); e.wait(r'Descomprimido:', m, 90)
check('copia descomprimida == original (md5)', md5(os.path.join(W, 'copia.txt')) == orig)
m = e.mark(); e.send('u doc.txt.huf copia.txt'); check('no pisa un destino existente', e.wait(r'ya existe', m, 5))

print('-- cancelacion --')
os.path.exists(os.path.join(W, 'doc.txt.huf')) and os.remove(os.path.join(W, 'doc.txt.huf'))
m = e.mark(); e.send('k'); time.sleep(0.2); e.send('n')
canc = e.wait(r'cancelad|Comprimido:', m, 30); time.sleep(0.3)
if re.search(r'cancelad', e.since(m)):
    check('n cancela y no deja .huf ni .huftmp', not os.path.exists(os.path.join(W, 'doc.txt.huf')) and not huerfanos())
else:
    print('  [----]  termino antes de cancelar: sube MB (no concluyente)')

print('-- salida con un trabajo activo --')
m = e.mark(); e.send('k'); time.sleep(0.15); e.send('q')
try: rc = e.p.wait(timeout=30)
except Exception: rc = None
check('q con trabajo activo: salida limpia (rc=0, sin colgarse) y sin temporales', rc == 0 and not huerfanos())

print(f'\nExitosas: {sum(res)}   Fallidas: {len(res) - sum(res)}')
subprocess.call(['rm', '-rf', W])
sys.exit(0 if all(res) else 1)
