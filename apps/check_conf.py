#!/usr/bin/env python3
"""Checks conf/mod_ollama_chat.conf.dist against the settings the code reads.

    python3 apps/check_conf.py                    every "OllamaChat.X" the code reads is in the conf
    python3 apps/check_conf.py OllamaChat.Journal.  ...and, for the keys that start so, the conf's
                                                  value is the code's own default

The conf is the module's documentation, and a setting the code reads that it
does not list is one nobody knows is there. Run from the module's root; it
exits 1 when something is missing or differs.

It reads the sources as text: a key is a string literal starting "OllamaChat."
that is followed by a comma, and its default the argument after it -- string
literals side by side are joined, true and false become 1 and 0. A key put
together at run time ("OllamaChat.Race.Phrases." + race) is not seen, and a
default that is a name rather than a literal cannot be compared, so the second
form is for a part's own settings, where they are plain.
"""
import glob
import os
import re
import sys

KEY = re.compile(r'"(OllamaChat\.[A-Za-z0-9_.]+)"\s*,')
ESCAPES = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'"}


def unescape(literal):
    out, i = [], 0
    while i < len(literal):
        if literal[i] == '\\' and i + 1 < len(literal):
            out.append(ESCAPES.get(literal[i + 1], '\\' + literal[i + 1]))
            i += 2
        else:
            out.append(literal[i])
            i += 1
    return ''.join(out)


def default_at(src, pos):
    """The argument that starts at src[pos], as the conf would write it."""
    i = pos
    while i < len(src) and src[i].isspace():
        i += 1
    if i < len(src) and src[i] == '"':
        parts = []
        while i < len(src) and src[i] == '"':
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            parts.append(unescape(src[i + 1:j]))
            i = j + 1
            while i < len(src) and src[i].isspace():
                i += 1
        return ''.join(parts)
    depth, j = 0, i
    while j < len(src):
        if src[j] in '(<{':
            depth += 1
        elif src[j] in ')>}':
            if depth == 0:
                break
            depth -= 1
        elif src[j] == ',' and depth == 0:
            break
        j += 1
    token = src[i:j].strip()
    if token in ('true', 'false'):
        return '1' if token == 'true' else '0'
    return token[:-1] if re.fullmatch(r'-?\d+(\.\d+)?f', token) else token


def code_defaults(root):
    found = {}
    for path in sorted(glob.glob(os.path.join(root, 'src', '*.cpp'))):
        src = open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
        for match in KEY.finditer(src):
            found.setdefault(match.group(1), default_at(src, match.end()))
    return found


def conf_values(root):
    conf = open(os.path.join(root, 'conf', 'mod_ollama_chat.conf.dist'), encoding='utf-8').read()
    values = {}
    for match in re.finditer(r'^(OllamaChat\.[A-Za-z0-9_.]+)[ \t]*=[ \t]*(.*)$', conf, re.M):
        value = match.group(2).strip()
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
            value = value[1:-1]
        values[match.group(1)] = value
    return values


def same(a, b):
    if a == b:
        return True
    try:
        return float(a) == float(b)
    except ValueError:
        return False


def main():
    root = os.getcwd()
    prefixes = sys.argv[1:]
    code, conf = code_defaults(root), conf_values(root)
    missing = sorted(key for key in code if key not in conf)
    print(f'{len(code)} settings read by the code, {len(conf)} in the conf.')
    for key in missing:
        print(f'  not in the conf: {key}')
    differ = []
    for key in sorted(code):
        if key in conf and any(key.startswith(p) for p in prefixes) and not same(code[key], conf[key]):
            differ.append(key)
            print(f'  {key}\n    code: {code[key][:100]}\n    conf: {conf[key][:100]}')
    if prefixes:
        checked = sum(1 for key in code if any(key.startswith(p) for p in prefixes))
        print(f'{checked} defaults compared, {len(differ)} differ.')
    sys.exit(1 if missing or differ else 0)


if __name__ == '__main__':
    main()
