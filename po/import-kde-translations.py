#!/usr/bin/env python3
"""Fill po/<lang>.po for Beluga from Dolphin's translations in this repository's history (the last
KDE commit, po/<lang>/dolphin.po), then from the KDE Frameworks and GTK catalogs installed in
/usr/share/locale.

Only messages whose text matches (ignoring KDE context, '&' and '_' accelerators and '...' vs '…')
are filled. printf placeholders in Beluga (%s, %d, ...) are matched to Qt's %1, %2. Translations whose
placeholders don't match are left out, so every file passes `msgfmt -c`.
Usage: po/import-kde-translations.py <repo> <min-coverage-percent>   (Beluga uses 30)
Overwrites po/<lang>.po. Translations written for Beluga go in po/extra/<lang>.po (same msgid, printf
placeholders); they are used for the messages KDE's catalogs don't have, so they survive a new import.
"""
import re, subprocess, sys, os

repo = sys.argv[1]
DOLPHIN_REV = 'cd71f21a7'  # last commit of KDE's Dolphin before the rewrite
min_cov = float(sys.argv[2])

def unq(s):
    s = s[1:-1]
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\'}.get(n, '\\' + n))
            i += 2
        else:
            out.append(c); i += 1
    return ''.join(out)

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'

def parse(text):
    entries, cur, key, comments = [], {}, None, []
    def flush():
        nonlocal cur, comments
        if 'msgid' in cur:
            cur['comments'] = comments
            entries.append(cur)
        cur, comments = {}, []
    for line in text.splitlines():
        if not line.strip():
            flush(); key = None; continue
        if line.startswith('#~'):
            continue
        if line.startswith('#'):
            if 'msgid' in cur: flush()
            comments.append(line); continue
        m = re.match(r'^(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', line)
        if m:
            key = m.group(1)
            if key == 'msgctxt' and 'msgid' in cur: flush()
            cur[key] = unq(m.group(2)); continue
        if line.startswith('"') and key:
            cur[key] += unq(line)
    flush()
    return entries

FMT = re.compile(r'%(?:\d+\$)?[-+ 0#]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[sdiufFeEgGxXocp]|%%')

def norm(s):
    s = s.replace('&&', '\0').replace('&', '').replace('\0', '&')
    return s.replace('...', '…').strip()

def to_qt(s):
    n = [0]
    def r(m):
        if m.group(0) == '%%': return '%'
        n[0] += 1
        return '%' + str(n[0])
    return FMT.sub(r, s), n[0]

def specs_of(t):
    """The printf conversions of t, or None when a '%' is not a valid directive."""
    out, i = [], 0
    pos = 0
    while True:
        j = t.find('%', pos)
        if j < 0: return out
        m = FMT.match(t, j)
        if not m: return None
        if m.group(0) != '%%': out.append(re.sub(r'^%\d+\$', '%', m.group(0)))
        pos = m.end()

def valid(msgid, t):
    if msgid.endswith('\n') != t.endswith('\n') or msgid.startswith('\n') != t.startswith('\n'): return False
    if '%' not in msgid: return '%' not in t
    a, b = specs_of(msgid), specs_of(t)
    if a is None and b is None:
        # Not a printf message (custom action placeholders such as %n, %U): the same % codes.
        return sorted(re.findall(r'%.', msgid)) == sorted(re.findall(r'%.', t))
    return a is not None and b is not None and sorted(a) == sorted(b)

pot = parse(open(os.path.join(repo, 'po/beluga.pot'), encoding='utf-8').read())
header, msgs = pot[0], pot[1:]
langs = subprocess.run(['git', '-C', repo, 'ls-tree', '--name-only', DOLPHIN_REV, 'po/'], capture_output=True, text=True).stdout.split()
report = []
for path in langs:
    lang = path.split('/')[1]
    try:
        src = subprocess.run(['git', '-C', repo, 'show', f'{DOLPHIN_REV}:po/{lang}/dolphin.po'], capture_output=True, text=True, check=True).stdout
    except subprocess.CalledProcessError:
        continue
    d = parse(src)
    dh = d[0]
    # Further sources, in order of preference: KDE Frameworks and GTK catalogs (LGPL) installed
    # on this machine. Dolphin took many of its strings (places, standard actions, the
    # properties dialog) from these, so Beluga's identical strings are translated there.
    sources = [d[1:]]
    for dom in ('kio6', 'kxmlgui6', 'kconfigwidgets6', 'kfilemetadata6', 'kwidgetsaddons6', 'gtk40'):
        mo = f'/usr/share/locale/{lang}/LC_MESSAGES/{dom}.mo'
        if not os.path.exists(mo): continue
        r = subprocess.run(['msgunfmt', mo], capture_output=True, text=True)
        if r.returncode: continue
        ents = parse(r.stdout)[1:]
        if dom == 'gtk40':
            mn = lambda t: re.sub(r'_(?=\w)', '', t)
            for e in ents:
                for k in list(e):
                    if k.startswith('msg') and k != 'msgctxt': e[k] = mn(e[k])
        sources.append(ents)
    table = {}
    for loose in (False, True):
        for ents in sources:
            for e in ents:
                if any('fuzzy' in c for c in e['comments'] if c.startswith('#,')): continue
                # kxmlgui also holds the standard actions of games ("Move" as a chess move).
                if re.search(r'game|chess|card', e.get('msgctxt', ''), re.I): continue
                if 'msgid_plural' in e:
                    if e.get('msgstr[0]') and not loose:
                        table.setdefault(('P', norm(e['msgid']), norm(e['msgid_plural'])), [e[k] for k in sorted(k for k in e if k.startswith('msgstr['))])
                elif e.get('msgstr'):
                    if not loose:
                        table.setdefault(norm(e['msgid']), e['msgstr'])
                    else:
                        # Different capitalisation, or a label that Beluga writes with a colon.
                        table.setdefault(('L', norm(e['msgid']).lower().rstrip(':')), e['msgstr'])
    # Translations of Beluga's own messages, written for Beluga (po/extra/<lang>.po): used where KDE's
    # catalogs have nothing.
    extra = {}
    xp = os.path.join(repo, 'po', 'extra', lang + '.po')
    if os.path.exists(xp):
        for x in parse(open(xp, encoding='utf-8').read())[1:]:
            if 'msgid_plural' in x:
                forms = [x[k] for k in sorted(k for k in x if k.startswith('msgstr['))]
                if all(forms): extra[(x['msgid'], x['msgid_plural'])] = forms
            elif x.get('msgstr'):
                extra[x['msgid']] = x['msgstr']
    out, done = [], 0
    for e in msgs:
        tr = None
        sid, n = to_qt(e['msgid'])
        if 'msgid_plural' in e:
            pid, _ = to_qt(e['msgid_plural'])
            got = table.get(('P', norm(sid), norm(pid)))
            if got:
                tr = []
                for t in got:
                    t2 = norm(t) if '&' in t else t.replace('...', '…')
                    tr.append(t2)
        else:
            got = table.get(norm(sid))
            if got is None:
                got = table.get(('L', norm(sid).lower().rstrip(':')))
                if got is not None:
                    got = got.rstrip(' :：')
                    if norm(sid).endswith(':'): got += ':'
            if got is not None:
                tr = norm(got) if '&' in got else got.replace('...', '…')
        if tr is not None and n:
            # Map %1.. back to the printf specifiers in order of their number.
            specs = [m.group(0) for m in FMT.finditer(e['msgid']) if m.group(0) != '%%']
            def back(t):
                idx = [int(x) for x in re.findall(r'%(\d)', t)]
                if sorted(set(idx)) != list(range(1, n + 1)) or len(idx) != n: return None
                if idx != sorted(idx):
                    # reordered: use positional printf specifiers
                    return re.sub(r'%(\d)', lambda m: '%' + m.group(1) + '$' + specs[int(m.group(1)) - 1][1:], t)
                return re.sub(r'%(\d)', lambda m: specs[int(m.group(1)) - 1], t)
            tr = [back(t) for t in tr] if isinstance(tr, list) else back(tr)
            if tr is None or (isinstance(tr, list) and None in tr): tr = None
        elif tr is not None:
            # Beluga prints messages without placeholders as they are, so a '%' must stay single,
            # but a Qt %1 means the translation belongs to a different message.
            ts = tr if isinstance(tr, list) else [tr]
            if any(re.search(r'%\d', t) for t in ts): tr = None
            elif '%' in e['msgid'] and any('%' in t for t in ts): tr = None
        if tr is not None:
            ts = tr if isinstance(tr, list) else [tr]
            if not all(valid(e['msgid'], t) for t in ts): tr = None
        if tr is None:
            tr = extra.get((e['msgid'], e['msgid_plural'])) if 'msgid_plural' in e else extra.get(e['msgid'])
            if tr is not None:
                ts = tr if isinstance(tr, list) else [tr]
                if not all(valid(e['msgid'], t) for t in ts): tr = None
        out.append((e, tr))
        if tr: done += 1
    cov = 100.0 * done / len(msgs)
    report.append((lang, done, cov))
    if cov < min_cov: continue
    plural = re.search(r'Plural-Forms: ([^\n]*)', dh['msgstr'])
    lines = ['# Beluga translation (%s).' % lang,
             '# Imported from KDE Dolphin\'s translation, which was written by:']
    lines += [c for c in dh['comments'] if c.startswith('#') and not c.startswith('#,') and 'DESCRIPTIVE' not in c and 'same license' not in c and c.strip() != '#']
    lines += ['# This file is distributed under the same license as the beluga package (GPL-2.0-or-later).',
              'msgid ""', 'msgstr ""',
              q('Project-Id-Version: beluga\n')]
    for field in ('PO-Revision-Date', 'Last-Translator', 'Language-Team'):
        m = re.search(field + r': ([^\n]*)', dh['msgstr'])
        lines.append(q('%s: %s\n' % (field, m.group(1) if m else '')))
    lines += [q('Language: %s\n' % lang),
              q('MIME-Version: 1.0\n'), q('Content-Type: text/plain; charset=UTF-8\n'),
              q('Content-Transfer-Encoding: 8bit\n')]
    if plural: lines.append(q('Plural-Forms: %s\n' % plural.group(1).replace('\\n', '').strip()))
    lines.append('')
    for e, tr in out:
        lines += [c for c in e['comments'] if c.startswith('#,')]
        lines.append('msgid ' + q(e['msgid']))
        if 'msgid_plural' in e:
            lines.append('msgid_plural ' + q(e['msgid_plural']))
            np = int(re.search(r'nplurals=(\d+)', plural.group(1)).group(1)) if plural else 2
            for i in range(np):
                t = tr[i] if tr and i < len(tr) else ''
                lines.append('msgstr[%d] %s' % (i, q(t)))
        else:
            lines.append('msgstr ' + q(tr or ''))
        lines.append('')
    open(os.path.join(repo, 'po', lang + '.po'), 'w', encoding='utf-8').write('\n'.join(lines))
for lang, done, cov in sorted(report, key=lambda r: -r[2]):
    print(f'{lang:8} {done:4} {cov:5.1f}%')
