"""Summarise a debugprint.log captured by sowprobe (read-only).

    python -I dpanalyze.py <debugprint.log> [--top N] [--category PREFIX] [--templates-only]

Per category: line count, threads, and the most common message templates (quoted text,
numbers and key=value values replaced by placeholders) with one real example each.
Line format: "[ms since hook] [thread id] [CATEGORY]: message".
"""
import re
import sys
from collections import Counter, defaultdict

LINE = re.compile(r'^\[\s*(\d+)\] \[\s*(\d+)\] (\[[^\]]*\]:) ?(.*)$')


ID = re.compile(r'\b[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]+\b')  # record names: Keep_Large, Relationship_Nemesis


def template(msg):
    """(template, values): record ids, names after FactionMember/Subject/Other, quoted
    text, key=value values and numbers become placeholders; ids are kept as values."""
    values = ID.findall(msg)
    t = ID.sub('<id>', msg)
    t = re.sub(r"'[^']*'", "'<s>'", t)
    t = re.sub(r'"[^"]*"', '"<s>"', t)
    t = re.sub(r'\b(FactionMember|Member|Subject|Other|Target|Captain)(\s*[=:]?\s*)(?!<)([^\s;,()]+)', r'\1\2<name>', t)
    t = re.sub(r'(\b[\w.]+\s*=\s*)(?!<)([^;,()=]+?)(?=\s*(;|,|\)|$))', r'\1<v>', t)
    t = re.sub(r'(?<![\w<])-?\d+(\.\d+)?(?![\w>])', '<n>', t)
    return t.strip(), values


def parse(path):
    out = []
    with open(path, 'rb') as f:
        for raw in f:
            text = raw.rstrip(b'\r\n')
            try:
                line = text.decode('utf-8')
            except UnicodeDecodeError:
                line = text.decode('cp1252', errors='replace')
            m = LINE.match(line)
            if m:
                out.append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))
    return out


def main(argv):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    path = argv[1]
    top = int(argv[argv.index('--top') + 1]) if '--top' in argv else 8
    only = argv[argv.index('--category') + 1] if '--category' in argv else None
    rows = parse(path)
    if not rows:
        print('no lines')
        return 1
    print(f'{path}: {len(rows)} lines, {rows[0][0] / 1000:.1f}s .. {rows[-1][0] / 1000:.1f}s after hook install')
    by_cat = defaultdict(list)
    for r in rows:
        by_cat[r[2]].append(r)
    for cat, items in sorted(by_cat.items(), key=lambda kv: -len(kv[1])):
        if only and not cat.startswith(only):
            continue
        threads = Counter(r[1] for r in items)
        print(f'\n{cat} {len(items)} lines; threads: ' + ', '.join(f'{t}({n})' for t, n in threads.most_common(4)))
        temps = Counter()
        example = {}
        ids = defaultdict(Counter)
        for r in items:
            t, values = template(r[3])
            temps[t] += 1
            example.setdefault(t, r[3])
            for v in values[:1]:
                ids[t][v] += 1
        for t, n in temps.most_common(top):
            print(f'  {n:6}  {t[:150]}')
            if ids[t]:
                print(f'          ids: ' + ', '.join(f'{v}({c})' for v, c in ids[t].most_common(6))
                      + (f' ... {len(ids[t]) - 6} more' if len(ids[t]) > 6 else ''))
            if '--templates-only' not in argv:
                print(f'          e.g. {example[t][:150]}')
        if len(temps) > top:
            print(f'          ... {len(temps) - top} more templates')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
