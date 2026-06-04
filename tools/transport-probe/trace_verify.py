import os,time,subprocess,threading,select
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
TR="/tmp/tv.log"
try: os.remove(TR)
except OSError: pass
# default thinking ON (matches the real server), small gen cap
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","8192","-n","200","--seed","7",
                    "--non-interactive","--trace",TR],
   cwd=DS4,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
out=bytearray(); err=bytearray(); stop=[False]
def pump():
    while not stop[0]:
        r,_,_=select.select([p.stdout,p.stderr],[],[],0.1)
        for f in r:
            d=os.read(f.fileno(),65536)
            if not d: continue
            (out if f is p.stdout else err).extend(d)
        if p.poll() is not None and not r: break
threading.Thread(target=pump,daemon=True).start()
def waitn(n,to):
    t=time.time()
    while time.time()-t<to:
        if bytes(err).count(b"+DWARFSTAR_WAITING")>=n: return True
        time.sleep(0.2)
    return False
waitn(1,600)
n=bytes(err).count(b"+DWARFSTAR_WAITING")
p.stdin.write(b"In exactly two sentences, explain what the ocean is. Then say DONE.\n"); p.stdin.flush()
waitn(n+1,200)
open("capture/tv_stdout.bin","wb").write(bytes(out))
print("=== GROUND TRUTH (clean non-interactive stdout) ===")
print(bytes(out).decode("utf-8","replace").strip()[:600])
print("=== trace size:", os.path.getsize(TR) if os.path.exists(TR) else 0)
stop[0]=True; p.terminate()
try: p.wait(timeout=5)
except: p.kill()
