#!/usr/bin/env python3
"""Fetch only hash-pinned public CC0 cymbal candidates. No model changes."""
from __future__ import annotations
import hashlib
import json
import pathlib
import urllib.parse
import urllib.request
from datetime import datetime, timezone

SOURCES = [
    ('1-vcsl-suspended', 'sgossner/VCSL', 'c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e', 'Idiophones/Struck Idiophones/Suspended Cymbal 1/', [
        ('susCymb1_hit_stick_pp1.wav', '012211003ea58fa9075c356592ead89e937ab96b', 1152978),
        ('susCymb1_hit_stick_f1.wav', '4529eca8c2b92f1b377b1eb0f55985152b245642', 1213358),
        ('susCymb1_hit_bell_mf1.wav', '3eee09cfd9268cf7a5b867ae08ea1e7df8735bb9', 1279550),
        ('susCymb1_hit_fff1.wav', 'b073dd7e3a6f871e0d550fc55d829a7158570a45', 2285114),
    ]),
    ('2-virtuosity-crash', 'sfzinstruments/virtuosity_drums', '9f04cf9a734527edfbb0a4eee1f674e45bbf71bc', 'Samples/oh/crash/', [
        ('oh_crash_crash_vl1_rr1.flac', '8cbe9cb0bc2d8d42703148c18828bd1b8b58f5d0', 1264401),
        ('oh_crash_crash_vl2_rr1.flac', 'b6af432414327be04e97d23bc6c5dd3cc1c3e439', 1426940),
        ('oh_crash_crash_vl3_rr1.flac', '464020e19ae66f3c6b1bedcb87877590f705cdff', 1628049),
    ]),
    ('3-virtuosity-ride', 'sfzinstruments/virtuosity_drums', '9f04cf9a734527edfbb0a4eee1f674e45bbf71bc', 'Samples/oh/ride/', [
        ('oh_ride_ride_vl1_rr1.flac', 'aff84ed9a051aabe7582efecfbe8f46ca35e24df', 1107361),
        ('oh_ride_ride_vl3_rr1.flac', '11b81972377915468bcf033785d313d2a7665b31', 945050),
        ('oh_ride_bell_vl2_rr1.flac', 'cb62785f95e29a240efd9c26ac2876b84ba5d504', 659602),
    ]),
]


def download(repo: str, ref: str, path: str, limit: int) -> tuple[str, bytes]:
    url = 'https://raw.githubusercontent.com/' + repo + '/' + ref + '/' + urllib.parse.quote(path, safe='/')
    request = urllib.request.Request(url, headers={'User-Agent': 'Faust-expr-cymbal-reference-audition'})
    with urllib.request.urlopen(request, timeout=35) as response:
        if urllib.parse.urlsplit(response.url).hostname != 'raw.githubusercontent.com':
            raise ValueError('Unexpected download host')
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError('Download exceeds declared size: ' + path)
    return url, data


def main() -> None:
    out = pathlib.Path('evidence/cymbal-references')
    out.mkdir(parents=True, exist_ok=True)
    records = []
    licensed = set()
    for group, repo, ref, prefix, files in SOURCES:
        target = out / group
        target.mkdir(exist_ok=True)
        if repo not in licensed:
            _, license_data = download(repo, ref, 'LICENSE', 100000)
            if b'CC0' not in license_data:
                raise ValueError('Expected CC0 licence text missing: ' + repo)
            (out / (repo.replace('/', '-') + '-LICENSE.txt')).write_bytes(license_data)
            licensed.add(repo)
        for name, expected, size in files:
            url, data = download(repo, ref, prefix + name, size)
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            if len(data) != size or blob != expected:
                raise ValueError('Source hash/size mismatch: ' + name)
            if not (data[:4] in (b'RIFF', b'fLaC')):
                raise ValueError('Not an expected audio file: ' + name)
            (target / name).write_bytes(data)
            records.append({'candidate': group, 'file': group + '/' + name, 'source_url': url,
                            'source_commit': ref, 'git_blob_sha1': blob, 'bytes': len(data),
                            'sha256': hashlib.sha256(data).hexdigest(), 'license': 'CC0-1.0'})
            print('Verified', group, name, len(data), flush=True)
    report = {'selected_reference': None, 'retrieved_utc': datetime.now(timezone.utc).isoformat(),
              'purpose': 'Candidate auditions only; wait for user choice before fitting any model.',
              'processing': 'Original upstream audio bytes, unchanged. No EQ, gain, truncation or resampling.',
              'files': records}
    (out / 'sources.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
