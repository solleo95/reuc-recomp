import re,bisect,collections,sys
syms=[]
for l in open('build/game/uc.map',encoding='utf-8',errors='replace'):
    m=re.match(r'\s*[0-9a-f]{4}:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$',l)
    if m: syms.append((int(m.group(2),16),m.group(1),m.group(3)))
syms.sort(); addrs=[s[0] for s in syms]
print('symbols',len(syms))
tot=0;byfn=collections.Counter();byobj=collections.Counter()
for l in open(sys.argv[1]):
    rva,n=l.split();rva=int(rva,16);n=int(n);va=0x140000000+rva
    i=bisect.bisect_right(addrs,va)-1
    name,obj=(syms[i][1],syms[i][2]) if i>=0 else ('?','?')
    if i<0 or va-addrs[i]>0x40000: name,obj='?far','?'
    tot+=n;byfn[name]+=n;byobj[re.sub(r'game_\d+','game_NNN',obj)]+=n
print('total samples',tot)
for k,v in byobj.most_common(14):print(f'{v/tot*100:5.1f}% {k}')
print()
for k,v in byfn.most_common(45):print(f'{v/tot*100:5.1f}% {k[:110]}')
