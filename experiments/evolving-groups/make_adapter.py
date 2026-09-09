#!/usr/bin/env python3
"""Two pinned adapters; original benchmark/renderer sources are immutable."""
from pathlib import Path
import hashlib,sys
PINS={'StableGroups.h':'f03195554da12a9744f2c6f47b6cbc0327c9ddff5cf0209221a0fe741700700a','groups_main.cpp':'45f07ecc0df379408585ed8de024ab2a90ca3c95786098ed3788afd0ee3215b2'}
def generate(old,out):
    texts={}
    for name,want in PINS.items():
        raw=(old/name).read_bytes()
        if hashlib.sha256(raw).hexdigest()!=want:raise ValueError('review changed prior source: '+name)
        texts[name]=raw.decode()
    a='else check(e.srcIndex<e.tgtIndex,"only forward internal DAG edges supported in this slice");'
    b='else check(std::find(b.members.begin(),b.members.end(),e.srcIndex)<std::find(b.members.begin(),b.members.end(),e.tgtIndex),"internal edge contradicts prepared member order");'
    s=texts['StableGroups.h']
    if s.count(a)!=1:raise ValueError('member-order adapter anchor')
    (out/'stable_dynamic.generated.h').write_text(s.replace(a,b))
    s=texts['groups_main.cpp']
    if s.count('int main(int argc,char**argv){')!=1 or s.count('#include "StableGroups.h"')!=1:raise ValueError('test entry adapter anchor')
    s=s.replace('int main(int argc,char**argv){','int stableGroupEntrypoint(int argc,char**argv){').replace('#include "StableGroups.h"','#include "stable_dynamic.generated.h"')
    (out/'groups_as_library.generated.h').write_text(s)
if __name__=='__main__':
    out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True);generate(Path(sys.argv[1]),out)
