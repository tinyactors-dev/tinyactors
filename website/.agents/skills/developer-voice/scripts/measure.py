"""Measure how prose sounds: sentence length and spread, address, contractions, numbers, headings.

Usage: python3 measure.py LABEL=PATH [LABEL=PATH ...]

Each PATH is a Markdown file or a directory searched for *.md (a built
page's index.md, a corpus of pages dumped as Markdown). Fenced code is
ignored; lines starting with "#" are headings, "- " list items, and any
other block of four words or more is prose, split into sentences.
"""
import pathlib
import re
import statistics
import sys

CONTRACTION = re.compile(r"\b\w+['’](t|re|ll|ve|d)\b|\b(it|that|here|what|there|let|who)['’]s\b", re.I)
HYPE = re.compile(r"\b(blazing|seamless(ly)?|powerful|effortless(ly)?|magic(al)?|simply|just|easy|easily|simple|instant(ly)?|radically|incredibl[ey]|robust|production-ready)\b", re.I)


def blocks(path):
    files = sorted(path.rglob('*.md')) if path.is_dir() else [path]
    for file in files:
        text = re.sub(r"```.*?```", "", file.read_text(encoding='utf-8'), flags=re.S)
        yield from (block.strip() for block in text.split('\n\n') if block.strip())


def measure(path):
    sentences, headings = [], []
    for block in blocks(path):
        if block.startswith('<!--') or block.startswith('>') or block.startswith('|'):
            continue
        if block.startswith('#'):
            headings.append(block.lstrip('#').strip())
            continue
        if block.startswith('- ') or len(block.split()) < 4:
            continue
        for sentence in re.split(r'(?<=[.!?])\s+(?=[A-Z"“(])', ' '.join(block.split())):
            if len(sentence.split()) >= 3:
                sentences.append(sentence)
    if not sentences:
        return None
    lengths = [len(s.split()) for s in sentences]
    count = len(sentences)
    share = lambda test: round(100 * sum(1 for s in sentences if test(s)) / count, 1)
    return {
        'sentences': count,
        'words per sentence, median': statistics.median(lengths),
        'spread (standard deviation)': round(statistics.pstdev(lengths), 1),
        '% under 8 words': share(lambda s: len(s.split()) < 8),
        '% over 25 words': share(lambda s: len(s.split()) > 25),
        '% with you': share(lambda s: re.search(r"\byou(r|rs)?\b", s, re.I)),
        '% with we, our, us': share(lambda s: re.search(r"\b(we|our|us)\b", s, re.I)),
        '% with a contraction': share(lambda s: CONTRACTION.search(s)),
        '% questions': share(lambda s: s.rstrip().endswith('?')),
        '% with a number': share(lambda s: re.search(r"\d", s)),
        '% with a hype word': share(lambda s: HYPE.search(s)),
        'headings': len(headings),
        'heading words, mean': round(statistics.mean(len(h.split()) for h in headings), 1) if headings else 0,
    }


def main(arguments):
    if not arguments:
        sys.exit(__doc__)
    results = {}
    for argument in arguments:
        label, _, path = argument.partition('=')
        result = measure(pathlib.Path(path))
        if result is None:
            sys.exit(f'{label}: no prose found in {path}')
        results[label] = result
    labels = list(results)
    print('| | ' + ' | '.join(labels) + ' |')
    print('|---|' + '---|' * len(labels))
    for key in next(iter(results.values())):
        print(f'| {key} | ' + ' | '.join(str(results[label][key]) for label in labels) + ' |')


if __name__ == '__main__':
    main(sys.argv[1:])
