"""Extend the hash-pinned materials kernel without changing its preserved source."""
from pathlib import Path
import argparse,hashlib
EXPECTED='3f7195af8cf3085239c2ef5a6df9a618ef6b29a6aea75413cb236069d64ff5f4'
def generate(old: str, controls: str) -> str:
 def replace_once(s,a,b):
  if s.count(a)!=1:raise ValueError('Baseline source contract changed: '+a[:60])
  return s.replace(a,b,1)
 pos=old.index('\nmode(');s=old[:pos]+'\n'+controls+old[pos:]
 s=replace_once(s,'mode(f,d,hr,hi,br,bi,sr,si) = (step ~ (_, _)) : (_,!) with {','mode(f,d,hr,hi,br,bi,sr,si) = (step ~ (_, _)) : (_,!) : *(1/sqrt(relative_thickness)) with {')
 s=replace_once(s,'  hz=f*frequency_scale*played_ratio;','''  old_hz=f*frequency_scale*played_ratio;
  free_hz=select2(relative_thickness==1,old_hz*thickness_played(f),old_hz);
  hz=select2(contact_pressure==0,pad_frequency(free_hz),free_hz);''')
 s=replace_once(s,'  md=select2(unchanged,md_calc,d);','''  old_md=select2(unchanged,md_calc,d);
  thick_md=max(1e-6,d+participation*(new_loss_thick(f*material_ratio*thickness_ratio(f))-rl));
  md=select2(relative_thickness==1,thick_md,old_md);
  held_loss=select2(contact_pressure==0,pad_decay(free_hz),0);''')
 s=replace_once(s,'(md*loss_scale+choke_loss+200*','(md*loss_scale+choke_loss+held_loss+200*')
 s=replace_once(s,'declare name "CYM-ID Materials - property-informed experiment";','declare name "CYM-ID Thickness and Contact - experimental";')
 return s
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('baseline',type=Path);p.add_argument('controls',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
 data=a.baseline.read_bytes()
 if EXPECTED and hashlib.sha256(data).hexdigest()!=EXPECTED:raise ValueError('Wrong baseline hash; preserve and review before upgrading')
 a.output.write_text(generate(data.decode(),a.controls.read_text()))
 print('kernel_sha256',hashlib.sha256(a.output.read_bytes()).hexdigest())
