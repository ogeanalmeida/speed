#!/usr/bin/env python3
"""Gera bike_speed_esp32/webapp.h com o app (comprimido) para o ESP32 servir pelo Wi-Fi.
Rode de novo sempre que mudar o app:   python3 make_webapp.py
"""
import gzip, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, '..')
OUT = os.path.join(HERE, 'bike_speed_esp32', 'webapp.h')

# (arquivo de origem, tipo, rotas no ESP32). panel.html fica em firmware/, o resto na pasta do app.
FILES = [
    ('panel.html', 'text/html; charset=utf-8', ['/', '/panel']),
    ('index.html', 'text/html; charset=utf-8', ['/app', '/index.html']),
    ('styles.css', 'text/css; charset=utf-8', ['/styles.css']),
    ('app.js', 'application/javascript; charset=utf-8', ['/app.js']),
    ('build.js', 'application/javascript; charset=utf-8', ['/build.js']),
    ('fonts/lexend-latin-400-normal.woff2', 'font/woff2', ['/fonts/lexend-latin-400-normal.woff2']),
    ('fonts/lexend-latin-500-normal.woff2', 'font/woff2', ['/fonts/lexend-latin-500-normal.woff2']),
    ('fonts/inter-latin-400-normal.woff2', 'font/woff2', ['/fonts/inter-latin-400-normal.woff2']),
]
def src(name): return os.path.join(HERE if name == 'panel.html' else APP, name)

lines = ['// Gerado por make_webapp.py. Nao edite a mao.', '#pragma once', '#include <Arduino.h>', '']
total = 0
sizes = []
for i, (name, mime, routes) in enumerate(FILES):
    raw = open(src(name), 'rb').read()
    isfont = name.endswith('.woff2')
    data = raw if isfont else gzip.compress(raw, 9, mtime=0)
    sizes.append(len(data))
    total += len(data)
    lines.append(f'const uint8_t WEB_DATA_{i}[] PROGMEM = {{{",".join(str(b) for b in data)}}};')
lines += ['', 'struct WebAsset { const char* path; const char* mime; const uint8_t* data; size_t len; bool gz; };',
          'const WebAsset WEB_ASSETS[] = {']
n = 0
for i, (name, mime, routes) in enumerate(FILES):
    for r in routes:
        lines.append(f'  {{"{r}", "{mime}", WEB_DATA_{i}, {sizes[i]}, {"false" if name.endswith(".woff2") else "true"}}},')
        n += 1
lines += ['};', f'const size_t WEB_ASSET_COUNT = {n};', '']
open(OUT, 'w').write('\n'.join(lines))
print(f'{OUT}: {len(FILES)} arquivos, {total/1024:.1f} KB no ESP32')
