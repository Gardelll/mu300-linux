#!/usr/bin/env python3
"""Which release the installers take, from GitHub's list of releases (GET /repos/<repo>/releases).

  tools/release_pick.py [FILE]          prints LATEST=<tag> and, only when there is one, BETA=<tag>
  tools/release_pick.py --json [FILE]   the same as one JSON object, with the files and sizes of both

FILE is the API's answer (default: standard input). LATEST is the newest full release: not a draft, not a
prerelease. BETA is the newest prerelease, and only when it was published after LATEST - an old prerelease that a
full release has overtaken is never offered. Anything that is not such a list (an empty answer, the error object of
a rate-limited or failed request, HTML of a proxy) gives no line at all, and the caller goes on as without a beta.
install.sh, install.ps1 and the graphical installer all decide here, so they cannot disagree.
"""
import json
import sys


def _when(rel):
    """What a release is ordered by: when it was published (ISO 8601 in UTC, so text order is time order)."""
    return str(rel.get('published_at') or rel.get('created_at') or '')


def _usable(rel):
    return isinstance(rel, dict) and isinstance(rel.get('tag_name'), str) and rel['tag_name'] and not rel.get('draft')


def _files(rel):
    out = {}
    for a in rel.get('assets') or []:
        if isinstance(a, dict) and isinstance(a.get('name'), str):
            try:
                out[a['name']] = int(a.get('size') or 0)
            except (TypeError, ValueError):
                out[a['name']] = 0
    return out


def pick(releases):
    """{'latest': tag or None, 'beta': tag or None, 'files': {tag: {name: bytes}}} from the parsed API answer."""
    res = {'latest': None, 'beta': None, 'files': {}}
    if not isinstance(releases, list):
        return res
    usable = [r for r in releases if _usable(r)]
    full = [r for r in usable if not r.get('prerelease')]
    if not full:
        return res
    latest = max(full, key=_when)
    res['latest'] = latest['tag_name']
    res['files'][latest['tag_name']] = _files(latest)
    pre = [r for r in usable if r.get('prerelease') and _when(r) > _when(latest) and r['tag_name'] != latest['tag_name']]
    if pre:
        beta = max(pre, key=_when)
        res['beta'] = beta['tag_name']
        res['files'][beta['tag_name']] = _files(beta)
    return res


def pick_text(text):
    """pick() of the API's answer as text; anything that does not parse is no release at all."""
    try:
        return pick(json.loads(text))
    except (ValueError, TypeError):
        return pick(None)


def plain(tag):
    """A tag goes into file names and URLs: one that is not plain is not passed on."""
    return bool(tag) and all(c.isascii() and (c.isalnum() or c in '._-+') for c in tag)


def main(argv):
    as_json = '--json' in argv
    paths = [a for a in argv if not a.startswith('--')]
    if paths:
        with open(paths[0], 'rb') as f:
            raw = f.read()
    else:
        raw = sys.stdin.buffer.read()
    res = pick_text(raw.decode('utf-8', 'replace'))
    for key in ('latest', 'beta'):
        if not plain(res[key]):
            res['files'].pop(res[key], None)
            res[key] = None
    if not res['latest']:
        res['beta'] = None
        res['files'] = {}
    if as_json:
        print(json.dumps(res, sort_keys=True))
        return 0
    for key in ('latest', 'beta'):
        if res[key]:
            print('%s=%s' % (key.upper(), res[key]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
