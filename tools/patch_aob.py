import sys, re, json
src, runtime, aobjson, incout = sys.argv[1:5]
s = open(src, encoding='utf-8').read()
res = json.load(open(aobjson, encoding='utf-8'))

# 1) signatures.inc
lines = ['/* Сгенерировано aobgen.py по "Roadcraft - Retail.exe" сборки 8.0.PATCH.772884. */',
         'static const GameSignature gameSignatures[]={']
for name, info in res.items():
    if 'pattern' not in info: sys.exit('нет шаблона для ' + name)
    kind = 'SIG_DIRECT' if info['kind'] == 'DIRECT' else 'SIG_RIPREL'
    lines.append(' {%s,"%s",%s,"%s",%d,%d,0x%x},' % (name, name, kind, info['pattern'], info['dispoff'], info['endoff'], info['rva']))
lines.append('};')
lines.append('#define KNOWN_SLOT_KEYBOARD_RVA 0x398f8f8')
open(incout, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')

# 2) жёсткие смещения -> gameAddr[...]
names = {info['rva']: name for name, info in res.items()}
names[0x398f8f8] = 'SLOT_KEYBOARD'
def sub(m):
    rva = int(m.group(1), 16)
    if rva not in names: sys.exit('неизвестное смещение 0x%x' % rva)
    return 'gameAddr[%s]' % names[rva]
s, n1 = re.subn(r'\(uintptr_t\)base\+0x([0-9a-f]+)', sub, s)
s, n2 = re.subn(r'\(base\+0x([0-9a-f]+)\)', sub, s)
left = re.findall(r'base\+0x[0-9a-f]+', s)
if left: sys.exit('остались жёсткие смещения: %r' % left)
print('replaced', n1, n2)

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count: sys.exit('expected %d got %d: %r' % (count, n, old[:80]))
    s = s.replace(old, new)

# 3) вставка поиска сигнатур и новая проверка версии
old_sig = 'static int signature(void){static BYTE expected[]={0x48,0x8b,0x89,0xd8,0,0,0,0xe9,0xd4,0xd4,3,0};BYTE actual[12];return readmem(gameAddr[FN_QUEUE],actual,12) && !memcmp(expected,actual,12);}'
rep('static WCHAR ini[MAX_PATH];\n', 'static WCHAR ini[MAX_PATH];\n' + open(runtime, encoding='utf-8').read())
new_sig = (
 '/* Версия подходит, если все адреса найдены по сигнатурам и функция очереди физики\n'
 '   по-прежнему берёт мир из поля +0xD8 (байты rel32 перехода не сравниваются). */\n'
 'static int signature(void){static const BYTE expected[]={0x48,0x8b,0x89,0xd8,0,0,0,0xe9};BYTE actual[8];return gameAddrReady && readmem(gameAddr[FN_QUEUE],actual,8) && !memcmp(expected,actual,8);}')
rep(old_sig, new_sig)
rep('if(!signature()){detectedTruck="Недоступно";strcpy(message,"Неподдерживаемая версия игры");return;}',
    'if(!signature()){detectedTruck="Недоступно";strcpy(message,gameAddrReady?"Неподдерживаемая версия игры":gameAddrError);return;}')
rep('lstrcatW(ini,L"KOTEuKA_CargoLoad.ini");',
    'lstrcatW(ini,L"KOTEuKA_CargoLoad.ini");\n'
    ' {WCHAR logPath[MAX_PATH];lstrcpyW(logPath,profilePath);lstrcpyW(logPath+lstrlenW(logPath)-8,L"log");\n'
    '  resolveGameAddresses();writeAddressLog(logPath);\n'
    '  if(!gameAddrReady)strcpy(message,gameAddrError);}')
rep('if(!signature()){*text=L"ВЕРСИЯ ИГРЫ НЕ ПОДДЕРЖИВАЕТСЯ";*color=COL_BAD;}',
    'if(!gameAddrReady){*text=L"АДРЕСА ИГРЫ НЕ НАЙДЕНЫ";*color=COL_BAD;}\n else if(!signature()){*text=L"ВЕРСИЯ ИГРЫ НЕ ПОДДЕРЖИВАЕТСЯ";*color=COL_BAD;}')
open(src, 'w', encoding='utf-8', newline='\n').write(s)
print('patched')
