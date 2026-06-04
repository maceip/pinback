import os,sys,time,subprocess,threading,select,re,pathlib
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","8192","-n","64","--nothink","--seed","1","--non-interactive"],
   cwd=DS4,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
out=bytearray(); err=bytearray(); stop=[False]
def pump():
    while not stop[0]:
        r,_,_=select.select([p.stdout,p.stderr],[],[],0.1)
        for f in r:
            b=os.read(f.fileno(),65536)
            if not b: continue
            (out if f is p.stdout else err).extend(b)
        if p.poll() is not None and not r: break
threading.Thread(target=pump,daemon=True).start()
def waitmark(n,to):
    t=time.time()
    while time.time()-t<to:
        if bytes(err).count(b"+DWARFSTAR_WAITING")>=n: return True
        time.sleep(0.2)
    return False
print("waiting for boot WAITING..."); waitmark(1,600)
print("BOOT +DWARFSTAR_WAITING:", bytes(err).count(b"+DWARFSTAR_WAITING"))
n=bytes(err).count(b"+DWARFSTAR_WAITING"); m=len(out)
p.stdin.write(b"reply with just the word ok\n"); p.stdin.flush()
waitmark(n+1,120)
seg=bytes(out[m:])
print("=== NON-INTERACTIVE assistant stream (raw repr) ===")
print(repr(seg))
print("=== escape chars present? ===", b"\x1b" in seg, " | CR present?", b"\r" in seg)
# now /save over non-interactive
n=bytes(err).count(b"+DWARFSTAR_WAITING"); m=len(out)
p.stdin.write(b"/save\n"); p.stdin.flush(); waitmark(n+1,120)
seg2=bytes(out[m:])
print("=== '/save' over non-interactive (raw repr) ===")
print(repr(seg2[:400]))
print("contains 'saved session'?", b"saved session" in seg2, "| sha?", bool(re.search(rb'saved session [0-9a-f]+',seg2)))
stop[0]=True; p.terminate()
try: p.wait(timeout=5)
except: p.kill()
