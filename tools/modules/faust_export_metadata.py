"""Repair metadata-only export portability in Faust 2.70.3.

Imported file paths become unquoted declaration keys; hyphens can make the
expanded DSP invalid. No signal expressions are changed. Preserve attribution,
restore canonical identity, and retain the original expansion in evidence.
"""
import re
DECL=re.compile(r'^declare\s+(\S+)\s+("(?:\\.|[^"\\])*")\s*;\s*$')
IDENTITY={'name','version','description'}

def normalize_expanded(expanded: str, original: str) -> str:
    identity={}
    for line in original.splitlines():
        m=DECL.fullmatch(line.strip())
        if m and m[1] in IDENTITY:identity[m[1]]=m[2]
    if not {'name','version'} <= identity.keys():
        raise ValueError('Canonical module needs explicit name and version')
    output=[f'declare {key} {value};' for key,value in identity.items()];used={}
    for line in expanded.splitlines():
        m=DECL.fullmatch(line.strip())
        if not m:
            output.append(line);continue
        key,value=m.groups()
        if key in IDENTITY or key in {'compile_options','filename'} or re.fullmatch(r'library_path\d+',key):continue
        if re.search(r'_voice_dsp_(?:name|version|description)$',key):continue
        safe=re.sub(r'[^A-Za-z0-9_]','_',key)
        if not safe or safe[0].isdigit():safe='metadata_'+safe
        if safe in used and used[safe]!=key:raise ValueError('Metadata key collision')
        used[safe]=key;output.append(f'declare {safe} {value};')
    result='\n'.join(output)+'\n'
    code=lambda text:[s for s in text.splitlines() if not DECL.fullmatch(s.strip())]
    if code(result)!=code(expanded):raise ValueError('Export normalization changed computation')
    return result
