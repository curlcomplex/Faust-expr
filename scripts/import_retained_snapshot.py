#!/usr/bin/env python3
"""One-time source publication, not CI execution or a private repository checkout.

Only an include-closure of the existing native benchmark is published. Temporary
source-download capabilities arrive encrypted to this run's ephemeral RSA key;
neither capabilities, private key, original archives nor unrelated source are
committed or uploaded. Ordinary benchmark CI remains read-only.
"""
import argparse, base64, hashlib, io, json, os, posixpath, re, subprocess, tarfile, time
import urllib.request, urllib.error, urllib.parse, zipfile
from pathlib import Path

NATIVE = '''clip-launcher/state/ClipStateContainer.cpp
control/surfaces/script/music/MusicData.cpp
control/surfaces/script/music/PitchUtils.cpp
control/surfaces/script/ScriptLanguage.cpp
control/surfaces/script/ScriptParser.cpp
control/surfaces/script/ScriptParserV2.cpp
control/surfaces/script/ScriptCompiler.cpp
graph/state/GraphState.cpp
graph/transport/FaustGraphRenderer.cpp
graph/transport/FaustGraphRendererAttachment.cpp
graph/transport/SignalDescriptor.cpp
graph/transport/PreparedSignalBlock.cpp
graph/transport/PreparedSignalSchedule.cpp
graph/transport/RealtimeWorkerTeam.cpp
modules/backend/FaustRuntime.cpp
modules/backend/FaustNode.cpp
io/CurlopEventLog.cpp'''.splitlines()
BENCH='scripts/bench/patching_latency/'
BRANCH='research/retained-authoring'
SOURCE_COMMIT='fc47759cf1bfd4126ea4484c91441c2190ba6237'
HARNESS_COMMIT='055c7a156e88e97d1db207769565fb6c7fcdff00'

def fetch(url,limit=40000000):
    parsed=urllib.parse.urlparse(url)
    if parsed.scheme!='https' or not (parsed.hostname=='raw.githubusercontent.com' or (parsed.hostname or '').endswith('.oaiusercontent.com')):
        raise RuntimeError('source endpoint not allowed')
    try:
        with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Faust-expr-scoped-import'}),timeout=45) as response:
            value=response.read(limit+1)
    except Exception:
        raise RuntimeError('source download failed; URL deliberately omitted') from None
    if len(value)>limit:raise RuntimeError('source download limit')
    return value

def git_sha(value):return hashlib.sha1(b'blob '+str(len(value)).encode()+b'\0'+value).hexdigest()
def checked_path(path):
    if path!=posixpath.normpath(path) or path.startswith(('/', '../')) or '\\' in path:
        raise RuntimeError('unsafe source path')
    return path

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--key',required=True);ap.add_argument('--public-key',required=True);args=ap.parse_args()
    if os.environ.get('GITHUB_REPOSITORY')!='curlcomplex/Faust-expr' or os.environ.get('GITHUB_REF')!='refs/heads/'+BRANCH:
        raise RuntimeError('wrong repository/ref')
    key_id=hashlib.sha256(Path(args.public_key).read_bytes()).hexdigest()
    envelope_url='https://raw.githubusercontent.com/curlcomplex/Faust-expr/'+BRANCH+'/.github/retained-import.encrypted.json'
    envelope=None
    for _ in range(90):
        try:
            candidate=json.loads(fetch(envelope_url,200000))
            if candidate.get('key_sha256')==key_id:envelope=candidate;break
        except (RuntimeError,ValueError):pass
        time.sleep(4)
    if envelope is None:raise RuntimeError('encrypted source manifest not received for this run')
    if not 1<=len(envelope['blocks'])<=256:raise RuntimeError('envelope block count')
    plaintext=[]
    for block in envelope['blocks']:
        result=subprocess.run(['openssl','pkeyutl','-decrypt','-inkey',args.key,'-pkeyopt','rsa_padding_mode:oaep','-pkeyopt','rsa_oaep_md:sha256'],input=base64.b64decode(block,validate=True),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        if result.returncode:raise RuntimeError('source envelope decryption failed')
        plaintext.append(result.stdout)
    payload=json.loads(b''.join(plaintext))
    archive=fetch(payload['archive']['url'])
    if hashlib.sha256(archive).hexdigest()!=payload['archive']['sha256']:raise RuntimeError('source archive checksum')
    members={}
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        inner=z.read('source-context.tar.gz')
    with tarfile.open(fileobj=io.BytesIO(inner),mode='r:gz') as t:
        # Read only C++ source/header members; never unpack the broader snapshot.
        for member in t:
            name=checked_path(member.name)
            if member.isfile() and name.startswith('native/') and Path(name).suffix in ('.h','.hpp','.cpp','.mm','.inc'):
                if member.size>2000000:raise RuntimeError('oversized source file')
                members[name]=t.extractfile(member).read()
    for item in payload['files']:
        name=checked_path(item['path'])
        allowed=(name.startswith(BENCH) and Path(name).suffix in ('.h','.cpp','.py','.md','.cmake')) or (name.startswith('scripts/v2-language/') and Path(name).suffix=='.mjs')
        if not allowed:raise RuntimeError('file outside authorized source classes')
        data=fetch(item['url'],2000000)
        if git_sha(data)!=item['git_sha']:raise RuntimeError('source blob checksum: '+name)
        members[name]=data
    for name in (BENCH+'main.cpp',BENCH+'retained_main.cpp',BENCH+'RetainedFaustGraph.h'):
        if name not in members:raise RuntimeError('missing benchmark entrypoint')
    selected={name for name in members if name.startswith((BENCH,'scripts/v2-language/'))}
    todo=['native/'+name for name in NATIVE]+list(selected)
    missing=set();visited=set()
    while todo:
        name=todo.pop()
        if name in visited:continue
        visited.add(name)
        if name not in members:raise RuntimeError('missing required source: '+name)
        selected.add(name)
        text=members[name].decode('utf-8')
        for include in re.findall(r'^\s*#\s*include\s+"([^"\n]+)"',text,re.M):
            alternatives=[posixpath.normpath(posixpath.join(posixpath.dirname(name),include)),'native/'+include,include]
            found=next((n for n in alternatives if n in members),None)
            if found:todo.append(found)
            elif include not in ('ScriptV2Catalogue.generated.inc','choc/containers/choc_SingleReaderSingleWriterFIFO.h'):
                missing.add(include)
    if missing:raise RuntimeError('unresolved local includes: '+','.join(sorted(missing)))
    # Reject recognizable embedded credentials; references to credential APIs are
    # source code, but actual bearer/private-key material must never be exported.
    sensitive=re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[0-9A-Z]{16}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|[?&]token=[A-Za-z0-9]{20,}|/Users/[^\s"\']+/\.ssh/)')
    for name in selected:
        if sensitive.search(members[name]):raise RuntimeError('potential embedded secret in '+name)
    destination=Path('vendor/curlop-latency')
    if destination.exists():raise RuntimeError('snapshot already exists; refusing overwrite')
    manifest={'source_repository':'curlcomplex/CURLOP','product_source_commit':SOURCE_COMMIT,'benchmark_source_commit':HARNESS_COMMIT,
              'authorization':'Owner explicitly permitted public publication of benchmark dependencies in chat, 2026-09-09.',
              'native_translation_units':NATIVE,'files':{}}
    for name in sorted(selected):
        data=members[name];path=destination/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        manifest['files'][name]={'sha256':hashlib.sha256(data).hexdigest(),'git_blob_sha1':git_sha(data),'bytes':len(data)}
    (destination/'SOURCE-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (destination/'NOTICE.md').write_text('# Authorized benchmark source snapshot\n\nThe repository owner explicitly approved publishing the native benchmark and its source dependencies. This is a selected source snapshot, not a repository/history export. Credentials, personal settings, recordings, unrelated assets and private Git history are excluded. Source copyrights remain with their owners; this notice does not infer a new license. Public third-party dependencies are fetched separately at pinned revisions.\n')
    # This is the requested source-publication operation, separate from read-only
    # test CI. Stage only the new approved subtree; do not force-push or merge.
    subprocess.run(['git','add','--','vendor/curlop-latency'],check=True)
    staged=subprocess.check_output(['git','diff','--cached','--name-only'],text=True).splitlines()
    if any(not path.startswith('vendor/curlop-latency/') for path in staged):raise RuntimeError('unexpected staged path')
    subprocess.run(['git','-c','user.name=github-actions[bot]','-c','user.email=41898282+github-actions[bot]@users.noreply.github.com','commit','-m','Publish owner-authorized CURLOP benchmark dependency snapshot'],check=True,stdout=subprocess.DEVNULL)
    token=os.environ['GH_TOKEN'];header='AUTHORIZATION: basic '+base64.b64encode(('x-access-token:'+token).encode()).decode()
    result=subprocess.run(['git','-c','http.https://github.com/.extraheader='+header,'push','origin','HEAD:refs/heads/'+BRANCH],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if result.returncode:raise RuntimeError('non-force source publication failed; no retry against changed branch')
    head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
    print('Published',len(selected),'authorized source files at',head)

if __name__=='__main__':main()
