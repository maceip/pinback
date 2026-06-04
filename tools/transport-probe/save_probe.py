import os,time,subprocess,threading,select,re
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
env=dict(os.environ); env.update({"LINENOISE_ASSUME_TTY":"1","TERM":"dumb"})
# match the server: large ctx, thinking ON
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","100000","-n","120","--seed","7"],
   cwd=DS4,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
out=bytearray(); stop=[False]; lock=threading.Lock(); seen=[0]
def raw(b):
    with lock: p.stdin.write(b); p.stdin.flush()
def pump():
    while not stop[0]:
        r,_,_=select.select([p.stdout,p.stderr],[],[],0.1)
        for f in r:
            d=os.read(f.fileno(),65536)
            if not d: continue
            if f is p.stdout:
                out.extend(d)
                tot=bytes(out).count(b"\x1b[6n")
                while seen[0]<tot: seen[0]+=1; raw(b"\x1b[1;200R")
        if p.poll() is not None and not r: break
threading.Thread(target=pump,daemon=True).start()
def wait(sub,to,after=0):
    t=time.time()
    while time.time()-t<to:
        i=bytes(out).find(sub,after); 
        if i>=0: return i
        time.sleep(0.1)
    return -1
wait(b"ds4-agent>",600); time.sleep(1)
mark=len(out)
raw(b"Remember this: the magic word is ZEBRA. Reply with just: ok\r")
# wait for turn: status returns to idle after generation
time.sleep(2)
t=time.time()
while time.time()-t<120:
    tail=bytes(out[-400:])
    s=re.sub(rb'\x1b\[[0-9;?]*[A-Za-z]',b'',tail)
    if b"| idle" in s and b"generation" not in s[-200:]: 
        time.sleep(1); break
    time.sleep(0.3)
print("=== turn done; now /save ===")
sm=len(out)
raw(b"/save\r")
# capture for 25s, looking for saved session / save scheduled
t=time.time(); found_saved=found_sched=False
while time.time()-t<25:
    seg=bytes(out[sm:])
    if b"saved session" in seg and not found_saved: found_saved=True; print("  [saw 'saved session' at +%.1fs]"%(time.time()-t))
    if b"save scheduled" in seg and not found_sched: found_sched=True; print("  [saw 'save scheduled' at +%.1fs]"%(time.time()-t))
    time.sleep(0.2)
seg=bytes(out[sm:])
clean=re.sub(rb'\x1b\[[0-9;?]*[A-Za-z]',b'',seg).replace(b'\r',b'\n')
# collapse status spam
lines=[l for l in clean.split(b'\n') if l.strip() and not (l.strip().startswith(b'ctx ') and b'idle' in l)]
print("=== /save output (cleaned, deduped tail) ===")
seen_l=[]
for l in lines[-25:]:
    if seen_l and seen_l[-1]==l: continue
    seen_l.append(l); print("  ", l.decode('utf-8','replace')[:120])
m=re.search(rb'saved session ([0-9a-f]+)', seg)
print("\nRESULT: saved-session-sha =", m.group(1).decode() if m else None)
stop[0]=True; raw(b"/quit\r"); time.sleep(0.5)
try: p.terminate(); p.wait(timeout=5)
except: p.kill()
