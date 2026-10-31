import json,collections,sys
d=json.load(open(sys.argv[1]))
r=d["results"]
ok={k:v for k,v in r.items() if "patterns" in v}
print("compared",len(ok),"errors",[k for k,v in r.items() if "patterns" not in v],"skipped",d["skipped"])
t=collections.Counter();off=[]
for k,v in ok.items():
    m=max(p["over8"] for p in v["patterns"]);n=max(p["over2"] for p in v["patterns"])
    tier="exact" if n<0.001 else "close" if m<0.01 else "off"
    t[tier]+=1;t[tier+":"+v.get("note","")]+=1
    if tier!="exact":off.append((round(m,3),round(n,3),round(max(p["worst"] for p in v["patterns"]),3),k,v["ucode"],v.get("note","")))
print(t)
for x in sorted(off,reverse=True)[:14]:print(x)
print("bool-sensitive",sum(1 for v in ok.values() if len({(p["over2"],p["worst"]) for p in v["patterns"]})>1))
