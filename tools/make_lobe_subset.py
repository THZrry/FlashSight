#!/usr/bin/env python3
"""Build the local Lobe Icons subset (web/icons/lobe/) for FlashSight.

Why a subset: the packaged app serves everything from inside the exe and the
desktop backend cannot fetch from the network, so brand icons must be local.
The upstream package ships 950 files (~600 standalone SVGs); we need ~60.

Source:  @lobehub/icons-static-svg 1.95.1  (MIT, LobeHub)
         https://registry.npmjs.org/@lobehub/icons-static-svg/-/icons-static-svg-1.95.1.tgz

Usage:
    curl -sS --ssl-no-revoke -o temp/lobe.tgz \
      https://registry.npmjs.org/@lobehub/icons-static-svg/-/icons-static-svg-1.95.1.tgz
    mkdir temp/lobe && tar -xzf temp/lobe.tgz -C temp/lobe
    python tools/make_lobe_subset.py temp/lobe/package/icons

The "-color" variants are preferred where they exist (they carry the brand
palette); otherwise the plain monochrome SVG is used. Both are fine for a UI
that renders them at ~14px, but colour reads better in a list.
"""
import io
import json
import os
import shutil
import sys
import xml.etree.ElementTree as ET

# key in our manifest -> (display name, candidate upstream basenames in order)
BRANDS = [
    ('openai',      'OpenAI',        ['openai']),
    ('claude',      'Claude',        ['claude', 'anthropic']),
    ('anthropic',   'Anthropic',     ['anthropic']),
    ('gemini',      'Gemini',        ['gemini']),
    ('google',      'Google',        ['google']),
    ('gemma',       'Gemma',         ['gemma']),
    ('deepseek',    'DeepSeek',      ['deepseek']),
    ('qwen',        'Qwen / 通义',    ['qwen']),
    ('alibaba',     '阿里云',         ['alibaba']),
    ('moonshot',    'Moonshot',      ['moonshot']),
    ('kimi',        'Kimi',          ['kimi']),
    ('zhipu',       '智谱 GLM',       ['zhipu', 'chatglm', 'glmv']),
    ('chatglm',     'ChatGLM',       ['chatglm']),
    ('meta',        'Meta / Llama',  ['meta']),
    ('mistral',     'Mistral',       ['mistral']),
    ('grok',        'Grok',          ['grok']),
    ('xai',         'xAI',           ['xai']),
    ('cohere',      'Cohere',        ['cohere']),
    ('perplexity',  'Perplexity',    ['perplexity']),
    ('groq',        'Groq',          ['groq']),
    ('together',    'Together',      ['together']),
    ('huggingface', 'Hugging Face',  ['huggingface']),
    ('ollama',      'Ollama',        ['ollama']),
    ('lmstudio',    'LM Studio',     ['lmstudio']),
    ('vllm',        'vLLM',          ['vllm']),
    ('openrouter',  'OpenRouter',    ['openrouter']),
    ('opencode',    'opencode',      ['opencode']),
    ('azure',       'Azure',         ['azure']),
    ('microsoft',   'Microsoft',     ['microsoft']),
    ('nvidia',      'NVIDIA',        ['nvidia']),
    ('aws',         'AWS',           ['aws']),
    ('bedrock',     'Bedrock',       ['bedrock']),
    ('vertexai',    'Vertex AI',     ['vertexai']),
    ('cloudflare',  'Cloudflare',    ['cloudflare']),
    ('fireworks',   'Fireworks',     ['fireworks']),
    ('cerebras',    'Cerebras',      ['cerebras']),
    ('sambanova',   'SambaNova',     ['sambanova']),
    ('novita',      'Novita',        ['novita']),
    ('replicate',   'Replicate',     ['replicate']),
    ('siliconcloud', 'SiliconCloud', ['siliconcloud']),
    ('baidu',       '百度',           ['baidu']),
    ('ernie',       '文心一言',       ['ernie', 'baidu']),
    ('bytedance',   '字节跳动',       ['bytedance']),
    ('doubao',      '豆包',           ['doubao']),
    ('tencent',     '腾讯',           ['tencent']),
    ('hunyuan',     '混元',           ['hunyuan']),
    ('minimax',     'MiniMax',       ['minimax']),
    ('stepfun',     '阶跃星辰',       ['stepfun']),
    ('internlm',    'InternLM',      ['internlm']),
    ('yi',          '零一万物',       ['yi']),
    ('baichuan',    '百川',           ['baichuan']),
    ('iflytek',     '科大讯飞',       ['iflytekcloud', 'iflytek']),
    ('spark',       '讯飞星火',       ['spark']),
    ('stability',   'Stability AI',  ['stability']),
    ('flux',        'FLUX',          ['flux']),
    ('midjourney',  'Midjourney',    ['midjourney']),
    ('ideogram',    'Ideogram',      ['ideogram']),
    ('runway',      'Runway',        ['runway']),
    ('recraft',     'Recraft',       ['recraft']),
    ('suno',        'Suno',          ['suno']),
    ('github',      'GitHub',        ['github']),
    ('vercel',      'Vercel',        ['vercel']),
    ('v0',          'v0',            ['v0']),
]

OUT_DIR = os.path.join('web', 'icons', 'lobe')


def pick(src_dir, candidates):
    """Prefer <name>-color.svg, then <name>.svg; None when neither exists."""
    for c in candidates:
        for suffix in ('-color', ''):
            p = os.path.join(src_dir, c + suffix + '.svg')
            if os.path.isfile(p):
                return p, c + suffix
    return None, None


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    src_dir = sys.argv[1]
    if not os.path.isdir(src_dir):
        sys.stderr.write('not a directory: %s\n' % src_dir)
        return 2

    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    manifest = {}
    copied = []
    bad = []
    for key, display, cands in BRANDS:
        src, upstream = pick(src_dir, cands)
        if not src:
            sys.stderr.write('MISSING %s (tried %s)\n' % (key, cands))
            continue
        dst = os.path.join(OUT_DIR, key + '.svg')
        shutil.copyfile(src, dst)
        try:
            ET.parse(dst)
        except Exception as e:
            bad.append('%s: %s' % (key, e))
            os.remove(dst)
            continue
        manifest[key] = display
        copied.append((key, upstream, os.path.getsize(dst)))

    io.open(os.path.join(OUT_DIR, 'manifest.json'), 'w', encoding='utf-8').write(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + '\n')

    total = sum(c[2] for c in copied)
    print('wrote %d icons to %s (%.1f KB)' % (len(copied), OUT_DIR, total / 1024.0))
    if bad:
        print('XML parse failures:', bad)
    return 0


if __name__ == '__main__':
    sys.exit(main())
