import os,time,subprocess,threading,select,re
DS4=os.path.expanduser("~/ds4");AGENT=DS4+"/ds4-agent";MODEL=DS4+"/ds4flash.gguf"
SHA="796344ed"   # 8-char prefix
env=dict(os.environ);env.update({"LINENOISE_ASSUME_TTY":"1","TERM":"dumb"})
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","100000","-n","120","--seed","9"],cwd=DS4,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
out=bytearray();stop=[False];lk=threading.Lock();seen=[0]
def raw(b):
    with lk: p.stdin.write(b);p.stdin.flush()
def pump():
    while not stop[0]:
        r,_,_=select.select([p.stdout,p.stderr],[],[],0.1)
        for f in r:
            d=os.read(f.fileno(),65536)
            if not d:continue
            if f is p.stdout:
                out.extend(d);tot=bytes(out).count(b"\x1b[6n")
                while seen[0]<tot:seen[0]+=1;raw(b"\x1b[1;200R")
        if p.poll() is not None and not r:break
threading.Thread(target=pump,daemon=True).start()
def wait(s,to,a=0):
    t=time.time()
    while time.time()-t<to:
        i=bytes(out).find(s,a)
        if i>=0:return i
        time.sleep(0.1)
    return -1
wait(b"ds4-agent>",600);time.sleep(2)
mk=len(out);raw(("/switch %s\r"%SHA).encode());time.sleep(4)
seg=bytes(out[mk:])
clean=re.sub(rb'\x1b\[[0-9;?]*[A-Za-z]',b'',seg).replace(b'\r',b'\n')
print("=== RAW /switch output (deduped non-status lines) ===")
last=None
for l in clean.split(b'\n'):
    l=l.rstrip()
    if not l.strip():continue
    if l.strip().startswith(b'ctx ') and b'idle' in l:continue
    if l==last:continue
    last=l;print("  ",l.decode('utf-8','replace')[:130])
stop[0]=True;raw(b"/quit\r");time.sleep(0.5)
try:p.terminate();p.wait(timeout=5)
except:p.kill()
