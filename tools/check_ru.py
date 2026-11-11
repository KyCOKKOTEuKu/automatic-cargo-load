import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctok
s = open(sys.argv[1], encoding='utf-8').read()
lines = s.split('\n')
left = 0
for a, b, w, t in ctok.string_literals(s):
    if not ctok.cyrillic(t): continue
    if s[max(0, a - 3):a] == 'TR(': continue
    ln = s.count('\n', 0, a) + 1
    print('%5d: %s' % (ln, lines[ln - 1].strip()[:110]))
    left += 1
print('без TR:', left)
