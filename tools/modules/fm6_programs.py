from pathlib import Path
import json
presets=[]
def op(n,level,coarse=1,fine=0,rates=(99,60,45,60),env=(99,75,65,0),velocity=0):
    p={f'op{n}_level':level,f'op{n}_coarse':coarse,f'op{n}_fine':fine,f'op{n}_velocity':velocity}
    p.update({f'op{n}_rate{i+1}':x for i,x in enumerate(rates)});p.update({f'op{n}_env{i+1}':x for i,x in enumerate(env)});return p

def add(id,title,controls,notes,step=.32,hold=.21,tail=1.5):presets.append(dict(id=id,title=title,controls=controls,notes=notes,step=step,hold=hold,tail=tail))
add('01-tine-keys','Tine Keys',{'algorithm':5}|op(1,87,rates=(95,48,35,55),env=(99,80,0,0),velocity=2)|op(2,80,1,rates=(99,62,45,65),env=(99,45,0,0),velocity=3)|op(3,73,1,rates=(99,58,45,60),env=(99,70,0,0),velocity=2)|op(4,62,14,rates=(99,72,50,65),env=(99,30,0,0),velocity=3)|op(5,64,1,rates=(99,50,45,55),env=(99,75,0,0))|op(6,68,1,rates=(99,64,45,55),env=(99,40,0,0)),[57,60,64,67,69,67,64,60])
add('02-rubber-bass','Rubber Bass',{'algorithm':1}|op(1,95,0,rates=(99,55,45,75),env=(99,80,70,0),velocity=1)|op(2,89,1,rates=(99,58,55,75),env=(99,45,30,0),velocity=2),[45,45,57,48,45,52,50,45],.24,.16,.65)
add('03-glass-bells','Glass Bells',{'algorithm':5}|op(1,87,1,rates=(99,45,34,42),env=(99,70,0,0),velocity=2)|op(2,82,3,50,rates=(99,55,40,45),env=(99,45,0,0))|op(3,75,2,37,rates=(99,50,40,45),env=(99,55,0,0))|op(4,79,11,rates=(99,58,40,45),env=(99,35,0,0))|op(5,65,4,10,rates=(99,50,40,45),env=(99,60,0,0))|op(6,69,7,rates=(99,60,40,45),env=(99,30,0,0)),[60,67,64,72,69,64],.44,.25,2.)
add('04-digital-brass','Digital Brass',{'algorithm':3}|op(1,87,1,rates=(70,65,40,65),env=(99,85,78,0),velocity=2)|op(2,80,1,rates=(75,66,45,65),env=(99,75,65,0))|op(3,68,1,rates=(90,60,40,65),env=(99,55,45,0))|op(4,74,2,rates=(70,65,40,65),env=(99,80,72,0))|op(5,64,1,rates=(75,60,40,65),env=(99,60,50,0))|op(6,51,3,rates=(85,60,40,65),env=(99,60,45,0)),[45,52,57,60,59,55],.4,.30,.8)
add('05-air-pad','Air Pad',{'algorithm':5}|op(1,83,1,rates=(55,45,35,42),env=(99,80,75,0))|op(2,75,1,rates=(60,50,40,45),env=(90,60,55,0))|op(3,70,2,rates=(52,45,35,42),env=(99,80,70,0))|op(4,65,3,rates=(60,55,40,45),env=(99,55,45,0))|op(5,62,4,rates=(50,45,35,42),env=(99,75,65,0))|op(6,60,5,rates=(60,55,40,45),env=(99,50,40,0)),[45,52,57,60],.95,.8,2.5)
add('06-wood-metal','Wood and Metal',{'algorithm':17}|op(1,91,1,rates=(99,65,55,75),env=(99,40,0,0),velocity=2)|op(2,79,2,rates=(99,74,65,75),env=(99,25,0,0))|op(3,69,5,rates=(99,80,65,75),env=(99,20,0,0))|op(4,68,7,rates=(99,75,65,75),env=(99,25,0,0))|op(5,60,11,rates=(99,80,65,75),env=(99,20,0,0))|op(6,65,3,rates=(99,70,60,75),env=(99,30,0,0)),[57,60,57,67,64,60,69,57],.22,.10,.6)
def write_presets(path):
    Path(path).write_text(json.dumps({'schema':1,'origin':'Original authored diagnostic/musical programs; no factory assets','presets':presets},indent=2)+'\n')
