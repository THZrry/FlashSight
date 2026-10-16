#!/usr/bin/env python3
"""Build the local file-type icon subset (web/icons/file/) for FlashSight.

Why a subset: the packaged app serves everything from inside the exe and the
desktop backend cannot fetch from the network, so icons must be local. The
upstream theme ships 1251 SVGs; a file tree needs a few dozen.

Source:  material-icon-theme 5.39.0  (MIT, Material Extensions)
         https://registry.npmjs.org/material-icon-theme/-/material-icon-theme-5.39.0.tgz
         The upstream dist/material-icons.json carries the authoritative
         extension -> icon mapping, so the manifest we emit is derived from it
         rather than hand-guessed (a guessed mapping is exactly how a .tsx file
         ends up with the TypeScript icon instead of the React one).

Usage:
    curl -sS --ssl-no-revoke -o temp/mit.tgz \
      https://registry.npmjs.org/material-icon-theme/-/material-icon-theme-5.39.0.tgz
    mkdir temp/mit && tar -xzf temp/mit.tgz -C temp/mit
    python tools/make_file_icons.py temp/mit/package

The emitted manifest maps BOTH extensions and exact filenames, because some
files are only identifiable by name (Dockerfile, Makefile, .gitignore).
"""
import io
import json
import os
import shutil
import sys
import xml.etree.ElementTree as ET

# Icon basenames we copy. Keys are the upstream icon names (dist json values).
# Everything a source tree in this project plausibly contains.
WANTED_ICONS = [
    'file', 'folder', 'folder-open',
    # languages
    'typescript', 'react_ts', 'javascript', 'react', 'python', 'csharp', 'cpp',
    'c', 'h', 'hpp', 'java', 'go', 'rust', 'ruby', 'php', 'swift', 'kotlin', 'lua',
    'dart', 'zig', 'elixir', 'scala', 'perl', 'haskell', 'clojure', 'r', 'vue',
    'svelte', 'webassembly', 'jupyter',
    # markup / config / data
    'html', 'css', 'sass', 'less', 'markdown', 'json', 'yaml', 'toml', 'xml',
    'database', 'settings', 'tune', 'console', 'powershell', 'cmake',
    'makefile', 'gradle', 'docker', 'git', 'lock', 'license', 'todo',
    'document', 'readme',
    # media / archives / binaries
    'image', 'pdf', 'zip', 'audio', 'video', 'font', 'log', 'word',
    'powerpoint', 'table',
]

# Extension -> icon, for the cases where the upstream mapping points at an icon
# we did not copy (we do not want 1381 files). Everything else comes straight
# from upstream's fileExtensions.
EXT_ALIASES = {
    'mdx': 'markdown', 'mdc': 'markdown',
    'sh': 'console', 'bash': 'console', 'zsh': 'console', 'fish': 'console',
    'bat': 'powershell', 'cmd': 'powershell', 'ps1': 'powershell', 'psm1': 'powershell',
    'jsx': 'react', 'tsx': 'react_ts',
    'mjs': 'javascript', 'cjs': 'javascript', 'mts': 'typescript', 'cts': 'typescript',
    'cc': 'cpp', 'cxx': 'cpp', 'c++': 'cpp', 'hh': 'hpp', 'hxx': 'hpp', 'h++': 'hpp',
    'rb': 'ruby', 'gemspec': 'ruby',
    'rs': 'rust',
    'kt': 'kotlin', 'kts': 'kotlin',
    'yml': 'yaml',
    'htm': 'html', 'xhtml': 'html',
    'scss': 'sass',
    'jpeg': 'image', 'jpg': 'image', 'png': 'image', 'gif': 'image', 'webp': 'image',
    'bmp': 'image', 'ico': 'image', 'tiff': 'image', 'avif': 'image',
    'svg': 'image',
    'mp3': 'audio', 'wav': 'audio', 'flac': 'audio', 'ogg': 'audio', 'm4a': 'audio',
    'mp4': 'video', 'mkv': 'video', 'mov': 'video', 'avi': 'video', 'webm': 'video',
    'ttf': 'font', 'otf': 'font', 'woff': 'font', 'woff2': 'font', 'eot': 'font',
    'gz': 'zip', 'bz2': 'zip', 'xz': 'zip', 'tar': 'zip', 'tgz': 'zip', '7z': 'zip',
    'rar': 'zip', 'zst': 'zip',
    'db': 'database', 'sqlite': 'database', 'sqlite3': 'database', 'sql': 'database',
    'csv': 'table', 'tsv': 'table',
    'xls': 'table', 'xlsx': 'table', 'ods': 'table',
    'doc': 'word', 'docx': 'word', 'odt': 'word', 'rtf': 'word',
    'ppt': 'powerpoint', 'pptx': 'powerpoint', 'odp': 'powerpoint',
    'txt': 'document', 'text': 'document', 'rst': 'document',
    'conf': 'settings', 'cfg': 'settings', 'properties': 'settings',
    'env': 'tune', 'editorconfig': 'settings',
    'wasm': 'webassembly', 'wat': 'webassembly',
    'ipynb': 'jupyter',
    'exe': 'console', 'dll': 'console', 'so': 'console', 'dylib': 'console',
    'bin': 'console', 'o': 'console', 'a': 'console', 'obj': 'console', 'lib': 'console',
    'log': 'log',
    'lock': 'lock', 'sum': 'lock',
    'gitignore': 'git', 'gitattributes': 'git', 'gitmodules': 'git',
    'dockerfile': 'docker', 'dockerignore': 'docker',
    'cmake': 'cmake', 'mk': 'makefile', 'gradle': 'gradle',
    'license': 'license', 'licence': 'license', 'copying': 'license',
    'todo': 'todo',
}

# Exact filename -> icon (name beats extension: "Dockerfile" has no extension).
NAME_MAP = {
    'dockerfile': 'docker', 'docker-compose.yml': 'docker', 'docker-compose.yaml': 'docker',
    '.dockerignore': 'docker',
    'makefile': 'makefile', 'gnumakefile': 'makefile',
    'cmakelists.txt': 'cmake',
    '.gitignore': 'git', '.gitattributes': 'git', '.gitmodules': 'git',
    'license': 'license', 'license.md': 'license', 'license.txt': 'license',
    'copying': 'license',
    'readme': 'readme', 'readme.md': 'readme', 'readme.txt': 'readme',
    '.env': 'tune', '.env.local': 'tune', '.env.example': 'tune',
    'cargo.toml': 'toml', 'cargo.lock': 'lock',
    'go.mod': 'go', 'go.sum': 'lock',
    'package.json': 'json', 'package-lock.json': 'lock', 'tsconfig.json': 'json',
    'xmake.lua': 'lua',
    'todo': 'todo', 'todo.md': 'todo',
}

OUT_DIR = os.path.join('web', 'icons', 'file')


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    pkg = sys.argv[1]
    src_icons = os.path.join(pkg, 'icons')
    mapping_path = os.path.join(pkg, 'dist', 'material-icons.json')
    if not os.path.isdir(src_icons) or not os.path.isfile(mapping_path):
        sys.stderr.write('expected <pkg>/icons and <pkg>/dist/material-icons.json\n')
        return 2

    upstream = json.load(io.open(mapping_path, encoding='utf-8'))
    up_ext = upstream.get('fileExtensions', {})
    up_names = upstream.get('fileNames', {})

    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    # 1) copy the icons we want
    copied = []
    for icon in WANTED_ICONS:
        src = os.path.join(src_icons, icon + '.svg')
        if not os.path.isfile(src):
            sys.stderr.write('MISSING icon %s\n' % icon)
            continue
        dst = os.path.join(OUT_DIR, icon + '.svg')
        shutil.copyfile(src, dst)
        try:
            ET.parse(dst)
        except Exception as e:
            sys.stderr.write('BAD XML %s: %s\n' % (icon, e))
            os.remove(dst)
            continue
        copied.append(icon)

    have = set(copied)

    # 2) build the extension map: start from upstream, keep only what we copied,
    #    then apply our aliases, then guarantee the common ones exist.
    ext_map = {}
    for ext, icon in up_ext.items():
        e = ext.lower().lstrip('.')
        if not e or ' ' in e or '/' in e:
            continue
        if icon in have:
            ext_map[e] = icon
    for ext, icon in EXT_ALIASES.items():
        if icon in have:
            ext_map[ext.lower().lstrip('.')] = icon

    name_map = {}
    for nm, icon in up_names.items():
        key = nm.lower()
        if icon in have and len(key) < 40:
            name_map[key] = icon
    for nm, icon in NAME_MAP.items():
        if icon in have:
            name_map[nm.lower()] = icon

    # 3) emit
    manifest = {
        'extensions': dict(sorted(ext_map.items())),
        'names': dict(sorted(name_map.items())),
        'folder': 'folder' if 'folder' in have else 'file',
        'folderOpen': 'folder-open' if 'folder-open' in have else 'folder',
        'fallback': 'file',
    }
    io.open(os.path.join(OUT_DIR, 'manifest.json'), 'w', encoding='utf-8').write(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + '\n')

    total = sum(os.path.getsize(os.path.join(OUT_DIR, i + '.svg')) for i in copied)
    print('copied %d icons (%.1f KB); %d extensions, %d filenames'
          % (len(copied), total / 1024.0, len(ext_map), len(name_map)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
