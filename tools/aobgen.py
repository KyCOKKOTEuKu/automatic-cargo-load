"""Генератор AOB-сигнатур для KOTEuKA_CargoLoad.
Для каждого жёстко заданного RVA ищет RIP-relative ссылки (и call/jmp rel32) в коде,
строит кратчайший уникальный шаблон с маской (rel32 заменяются на ??) и печатает C-таблицу."""
import struct, sys, re, json
import numpy as np

EXE = sys.argv[1]
d = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
imagebase = struct.unpack_from('<Q', d, opt + 24)[0]
sizeofimage = struct.unpack_from('<I', d, opt + 56)[0]
secs = []
for i in range(nsec):
    o = opt + optsz + 40 * i
    name = d[o:o + 8].rstrip(b'\0').decode()
    vs, va, rs, raw = struct.unpack_from('<IIII', d, o + 8)
    ch = struct.unpack_from('<I', d, o + 36)[0]
    secs.append((name, va, vs, raw, rs, ch))
print('imagebase', hex(imagebase), 'sizeofimage', hex(sizeofimage), file=sys.stderr)
for s in secs: print('  %-8s va=%08x vs=%08x raw=%08x rs=%08x ch=%08x' % s, file=sys.stderr)

def sec_of(rva):
    for s in secs:
        if s[1] <= rva < s[1] + max(s[2], s[4]): return s
code = [s for s in secs if s[0] == ".text"]
assert len(code) == 1, code
cname, cva, cvs, craw, crs, _ = code[0]
text = d[craw:craw + min(cvs, crs)]
print('code section', cname, hex(cva), len(text), file=sys.stderr)

def rva2off(rva):
    s = sec_of(rva)
    return s[3] + rva - s[1]

TARGETS = {
 'FN_QUEUE':       0x2cd40c0, 'FN_MOTION':      0x2cd8810,
 'FN_KEYBOARD':    0x1daac50, 'FN_CAMERA_CTOR': 0x9e5d80,
 'VT_SHAPE_WRAP':  0x3ea8b38, 'VT_SHAPE_HULL':  0x3ea9970,
 'VT_SHAPE_LIST':  0x3ea5808, 'VT_SHAPE_CYL':   0x3ea9b10,
 'VT_SHAPE_CAPS':  0x3ea8f30, 'VT_BODY':        0x3eaf290,
 'VT_CONSTRAINT':  0x3eae7d8, 'VT_CAMERA':      0x38c66e0,
 'G_WORLD':        0x4828a70, 'G_ALLOC':        0x59d9a20,
 'G_PAD_GETSTATE': 0x58114c8,
}

def looks_rel(buf, q, qrva):
    v = struct.unpack_from('<i', buf, q)[0]
    return abs(v) >= 0x10000 and 0 <= qrva + 4 + v < sizeofimage

def mask_window(start_rva, length, force):
    """bytes + mask для окна кода; force — набор смещений (в окне), которые обязательно ??."""
    # Анализируем окно с запасом 4 байта с каждой стороны, чтобы маскировать
    # и rel32, которые лишь частично попадают в окно.
    off = start_rva - cva
    if off < 4: return None
    ext = text[off - 4:off + length + 4]
    if len(ext) < length + 8: return None
    em = [True] * (length + 8)
    for f in force:
        for k in range(4):
            em[4 + f + k] = False
    q = 0
    while q + 4 <= length + 8:
        if all(em[q:q + 4]) and looks_rel(ext, q, start_rva - 4 + q):
            for k in range(4): em[q + k] = False
            q += 4
        else:
            q += 1
    return ext[4:4 + length], em[4:4 + length]

def to_regex(buf, m):
    return re.compile(b''.join(re.escape(bytes([b])) if k else b'.' for b, k in zip(buf, m)), re.DOTALL)

def count(buf, m, limit=2):
    rx = to_regex(buf, m)
    n = 0
    for _ in rx.finditer(text):
        n += 1
        if n >= limit: break
    return n

def refs_to(target):
    out = []
    n = len(text)
    for s in range(4):
        cnt = (n - s) // 4
        arr = np.frombuffer(text, dtype='<i4', count=cnt, offset=s).astype(np.int64)
        pos = cva + s + 4 * np.arange(cnt, dtype=np.int64)
        for k in (0, 1, 4):
            hit = np.nonzero(arr + pos + 4 + k == target)[0]
            for h in hit:
                out.append((int(pos[h]), 4 + k))   # rva поля disp, длина до конца инструкции
    return sorted(set(out))

def best_for_ref(p, endoff):
    """Наращиваем окно вокруг ссылки, пока шаблон не станет уникальным."""
    best = None
    for before in (8, 10, 12, 14, 16, 20, 24, 28):
        for after in (8, 12, 16, 20, 24, 28):
            start = p - before
            length = before + endoff + after
            r = mask_window(start, length, [before])
            if not r: continue
            buf, m = r
            if not m[0] or not m[-1]: continue
            fixed = sum(m)
            if length < 24 or fixed < 16: continue
            if count(buf, m) == 1:
                cand = (length, start, buf, m, before, before + endoff)
                if best is None or cand[0] < best[0]: best = cand
                break
        if best and best[0] <= before + endoff + 12: break
    return best

def best_direct(target):
    for length in (24, 28, 32, 40, 48, 64):
        r = mask_window(target, length, [])
        if not r: continue
        buf, m = r
        if sum(m) >= 16 and count(buf, m) == 1:
            return (length, target, buf, m)
    return None

results = {}
for name, t in TARGETS.items():
    sec = sec_of(t)
    refs = refs_to(t)
    info = {'rva': t, 'section': sec[0], 'refs': len(refs)}
    cands = []
    if name.startswith('FN_'):
        dct = best_direct(t)
        if dct:
            cands.append(('DIRECT', dct[0], dct[2], dct[3], 0, 0, dct[1]))
    for p, endoff in refs[:60]:
        b = best_for_ref(p, endoff)
        if b:
            cands.append(('RIPREL', b[0], b[2], b[3], b[4], b[5], b[1]))
    cands.sort(key=lambda c: (c[1], c[0] != 'DIRECT'))
    if not cands:
        info['error'] = 'нет уникального шаблона'
    else:
        kind, length, buf, m, dispoff, endoff, start = cands[0]
        info.update(kind=kind, pattern=' '.join('%02X' % b if k else '??' for b, k in zip(buf, m)),
                    dispoff=dispoff, endoff=endoff, match_rva=start, candidates=len(cands))
    results[name] = info
    print(name, json.dumps({k: v for k, v in info.items() if k != 'pattern'}), file=sys.stderr)

# vtable-слот клавиатуры: qword = imagebase + FN_KEYBOARD
slot = 0x398f8f8
val = struct.unpack_from('<Q', d, rva2off(slot))[0]
needle = struct.pack('<Q', imagebase + TARGETS['FN_KEYBOARD'])
occ = []
for s in secs:
    if s[5] & 0x20000000: continue
    blob = d[s[3]:s[3] + s[4]]
    i = blob.find(needle)
    while i >= 0:
        if i % 8 == 0: occ.append(s[1] + i)
        i = blob.find(needle, i + 1)
print('KEYBOARD_SLOT value ok:', val == imagebase + TARGETS['FN_KEYBOARD'], 'aligned occurrences:', [hex(x) for x in occ], file=sys.stderr)
json.dump(results, open(sys.argv[2], 'w'), indent=1)
