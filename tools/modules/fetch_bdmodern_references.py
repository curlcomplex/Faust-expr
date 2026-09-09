"""Acquire six explicitly licensed public BD Modern listening references.

These are the creator's public MP3 previews, NOT lossless hardware captures.
No checkout, account, cookies, payment, credentials or access bypass is used.
Unknown patch/firmware/gain fields remain unknown. Run only as an intentional
reference-acquisition step; ordinary DSP tests must not depend on this website.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import subprocess
from urllib.parse import urlparse, urljoin
from urllib.request import Request, build_opener, HTTPRedirectHandler

ALBUM = 'https://particlesintowaves.bandcamp.com/album/syntakt-designer-drums'
LICENSE = 'https://creativecommons.org/licenses/by/4.0/'
# Fixed before inspecting or fitting audio; do not cherry-pick by best fit.
SELECTION = ('BDM-01', 'BDM-05', 'BDM-09', 'BDM-14', 'BDM-20', 'BDM-26')


def approved(url: str) -> str:
    p = urlparse(url)
    if p.scheme != 'https' or p.username or p.password or p.port not in (None, 443):
        raise ValueError('Only unauthenticated HTTPS is allowed')
    host = (p.hostname or '').lower()
    if host != 'particlesintowaves.bandcamp.com' and not host.endswith('.bcbits.com'):
        raise ValueError('Unexpected reference host: ' + host)
    return url


class SafeRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return super().redirect_request(req, fp, code, msg, headers, approved(newurl))


def get(url: str, limit: int) -> bytes:
    req = Request(approved(url), headers={'User-Agent': 'Faust-expr-reference-research/1.0'})
    with build_opener(SafeRedirect()).open(req, timeout=30) as response:
        approved(response.url)
        data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError('Reference exceeded download bound')
        return data


class AlbumParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.album = None
    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key == 'data-tralbum':
                self.album = json.loads(value)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def acquire(out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    html = get(ALBUM, 4_000_000)
    text = html.decode('utf-8')
    # Fail closed if the creator's previously reviewed license disappears.
    if 'creativecommons.org/licenses/by/4.0' not in text:
        raise ValueError('Expected creator CC BY 4.0 declaration missing')
    (out / 'source-page.html').write_bytes(html)
    parser = AlbumParser(); parser.feed(text)
    if not parser.album:
        raise ValueError('No public track metadata in creator page')
    tracks = {row['title']: row for row in parser.album['trackinfo']}
    records = []
    for name in SELECTION:
        track = tracks[name]
        media = track.get('file', {}).get('mp3-128')
        if not media:
            raise ValueError('Missing public preview for ' + name)
        if media.startswith('//'):
            media = 'https:' + media
        if media.startswith('http:'):
            media = 'https:' + media[5:]
        raw = get(media, 2_000_000)
        mp3 = out / (name + '.mp3'); mp3.write_bytes(raw)
        info = json.loads(subprocess.check_output([
            'ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(mp3)], text=True))
        streams = [s for s in info['streams'] if s['codec_type'] == 'audio']
        if len(streams) != 1 or int(streams[0]['channels']) not in (1, 2):
            raise ValueError('Unsupported audio stream')
        if float(info['format']['duration']) > 15:
            raise ValueError('Unexpected reference duration')
        wav = out / (name + '.wav')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(mp3),
                        '-map', '0:a:0', '-c:a', 'pcm_f32le', str(wav)], check=True, timeout=30)
        records.append({
            'id': name, 'title': name, 'track_id': track['track_id'],
            'source_page': urljoin(ALBUM, track['title_link']),
            'media_url': media, 'original_file': mp3.name, 'sha256': sha(raw),
            'decoded_file': wav.name, 'decoded_sha256': sha(wav.read_bytes()),
            'codec': streams[0]['codec_name'], 'sample_rate': int(streams[0]['sample_rate']),
            'channels': int(streams[0]['channels']), 'format': info['format'],
            'conversion': 'ffmpeg decode to float32 WAV; no resample, gain, trim, EQ or normalization',
            'hardware': 'Elektron Syntakt', 'machine': 'BD Modern (creator BDM naming)',
            'firmware': None, 'parameters': None, 'note_velocity': None,
            'recording_gain': None, 'drive_used_on_this_hit': None,
            'eligible_for': 'preliminary sample-level timbre/envelope study only',
            'not_eligible_for': 'exact waveform, hidden algorithm, knob mapping, velocity law or latest-firmware validation'
        })
    report = {
        'schema': 1, 'source': ALBUM, 'creator': 'Winston Edwards / Particles Into Waves',
        'collection': 'Syntakt Designer Drums', 'release_date': '2022-06-13',
        'license': 'CC BY 4.0', 'license_url': LICENSE,
        'source_page_sha256': sha(html), 'retrieved_utc': datetime.now(timezone.utc).isoformat(),
        'creator_description': 'Recorded dry without post processing; some hits use onboard drive. Per-hit drive settings not supplied.',
        'fidelity_limit': 'Public lossy MP3 previews, not the offered original 16-bit/48kHz WAV download. Firmware and controls unknown.',
        'selection_before_audio_inspection': list(SELECTION),
        'ffmpeg_version': subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
        'records': records
    }
    (out / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    (out / 'ATTRIBUTION.md').write_text(
        '# Reference audio attribution\n\n'
        'BDM-01, BDM-05, BDM-09, BDM-14, BDM-20 and BDM-26 from **Syntakt Designer Drums**, '
        'Winston Edwards / Particles Into Waves, released 13 June 2022.\n\n'
        f'Source: {ALBUM}\nLicense: Creative Commons Attribution 4.0 International ({LICENSE}).\n\n'
        'These files are the publicly served MP3 listening previews and float32 WAV decodes, '
        'not the original lossless download. No gain, trim, normalization or EQ was applied during acquisition. '
        'Later comparison edits must be documented separately. No endorsement by the creator or Elektron is implied.\n')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    report = acquire(args.out.resolve())
    print(json.dumps({'references': len(report['records']), 'license': report['license'],
                      'quality': 'lossy preliminary references, not controlled captures'}))
