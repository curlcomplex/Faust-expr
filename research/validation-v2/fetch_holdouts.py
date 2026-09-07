"""Fetch additional immutable CC0 hits for validation, never as synthesis input."""
from pathlib import Path
import sys,json,hashlib
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
from fetch_cymbal_references import download
DATA=[
 ('1-vcsl-suspended','sgossner/VCSL','c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e','Idiophones/Struck Idiophones/Suspended Cymbal 1/',[
 ('susCymb1_hit_stick_mp1.wav','22733b4388f26829d94de380711792f7a56ab4da',1399926),
 ('susCymb1_hit_bell_pp1.wav','1d07e2596fd7efc7d9117cf64ef2afb3ea4ef488',1300206),
 ('susCymb1_hit_bell_fff1.wav','103cfc469b1ef5533e5d2f3e458241dfd0043345',748994)]),
 ('2-virtuosity-crash','sfzinstruments/virtuosity_drums','9f04cf9a734527edfbb0a4eee1f674e45bbf71bc','Samples/oh/crash/',[
 ('oh_crash_crash_vl3_rr2.flac','f80c919d56e5a7229b6eec4a06a8a5b5b4739ba7',1585868)]),
 ('3-virtuosity-ride','sfzinstruments/virtuosity_drums','9f04cf9a734527edfbb0a4eee1f674e45bbf71bc','Samples/oh/ride/',[
 ('oh_ride_ride_vl3_rr2.flac','7eb3b0ef766a8fe81cb6e7c9517a295731982a04',803470)])]
def main():
 root=Path('evidence/validation-v2/holdouts');root.mkdir(parents=True,exist_ok=True);records=[]
 for group,repo,ref,prefix,files in DATA:
  out=root/group;out.mkdir(exist_ok=True)
  _,lic=download(repo,ref,'LICENSE',100000)
  if b'CC0' not in lic:raise ValueError('Licence changed')
  (root/(repo.replace('/','-')+'-LICENSE.txt')).write_bytes(lic)
  for name,sha,n in files:
   url,data=download(repo,ref,prefix+name,n)
   actual=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
   if len(data)!=n or actual!=sha:raise ValueError('Source identity mismatch '+name)
   (out/name).write_bytes(data);records.append({'file':group+'/'+name,'source_url':url,'git_blob_sha1':actual,'sha256':hashlib.sha256(data).hexdigest(),'bytes':n,'license':'CC0-1.0'})
 (root/'manifest.json').write_text(json.dumps({'purpose':'Independent validation hits and real-versus-real repeat variability. Not used to select the first dense-contact research parameters.','files':records},indent=2)+'\n')
if __name__=='__main__':main()
