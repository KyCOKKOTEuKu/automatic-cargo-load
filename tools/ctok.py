"""Мини-разбор C: находит строковые литералы (с префиксом L или без), пропуская комментарии и символьные литералы."""
import re

def string_literals(s):
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if s.startswith('//', i):
            j = s.find('\n', i); i = n if j < 0 else j
        elif s.startswith('/*', i):
            j = s.find('*/', i + 2); i = n if j < 0 else j + 2
        elif c == "'" or (c == 'L' and i + 1 < n and s[i + 1] == "'" and not (s[i - 1].isalnum() or s[i - 1] == '_')):
            j = i + (2 if c == 'L' else 1)
            while j < n and s[j] != "'":
                j += 2 if s[j] == '\\' else 1
            i = j + 1
        elif c == '"' or (c == 'L' and i + 1 < n and s[i + 1] == '"' and not (s[i - 1].isalnum() or s[i - 1] == '_')):
            start = i
            j = i + (2 if c == 'L' else 1)
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            out.append((start, j + 1, c == 'L', s[start + (2 if c == 'L' else 1):j]))
            i = j + 1
        else:
            i += 1
    return out

def cyrillic(t):
    return re.search('[А-Яа-яЁё]', t) is not None
