#!/usr/bin/env python3
"""Check local inline Markdown links and heading anchors, without network access."""

from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def content(path):
    # Code examples are not documentation navigation.
    return re.sub(r'^\s*(`{3,}|~{3,}).*?^\s*\1\s*$', '',
                  path.read_text(encoding='utf-8'), flags=re.M | re.S)


def anchors(path):
    counts = {}
    result = set()
    for heading in re.findall(r'^#{1,6}\s+(.+?)\s*#*$', content(path), re.M):
        slug = re.sub(r'[^\w\- ]', '', heading.replace('`', '').lower()).replace(' ', '-')
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        result.add(slug + (f'-{count}' if count else ''))
    result.update(re.findall(r'(?:id|name)=["\']([^"\']+)', content(path)))
    return result


def main():
    paths = [ROOT / 'README.md', ROOT / 'AGENTS.md']
    paths += sorted((ROOT / 'docs').rglob('*.md'))
    paths += [ROOT / role / 'AGENTS.md' for role in ['app', 'server', 'lib']]
    errors = []
    checked = 0
    for path in paths:
        for match in re.finditer(r'!?\[[^\]\n]*\]\((<[^>]+>|[^\s)]+)(?:\s+"[^"]*")?\)', content(path)):
            destination = unquote(match.group(1).strip('<>'))
            url = urlsplit(destination)
            if url.scheme or url.netloc:
                continue
            target = (path.parent / url.path).resolve() if url.path else path
            checked += 1
            if not target.exists():
                errors.append(f'{path.relative_to(ROOT)}: missing {destination}')
            elif url.fragment and target.suffix == '.md' and url.fragment not in anchors(target):
                errors.append(f'{path.relative_to(ROOT)}: missing anchor {destination}')
    print(f'Checked {checked} local inline Markdown links.')
    print('\n'.join(errors) if errors else 'Documentation links passed.')
    return bool(errors)


if __name__ == '__main__':
    sys.exit(main())
