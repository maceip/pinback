import os,time,subprocess,threading,select
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","8192","-n","220","--nothink","--seed","3","--non-interactive"],
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
n=bytes(err).count(b"+DWARFSTAR_WAITING"); mark=len(out)
p.stdin.write(b"Create a file at /tmp/pinback_probe2.txt containing exactly the word banana. Use your file tool.\n"); p.stdin.flush()
waitn(n+1,150)
seg=bytes(out[mark:])
open("capture/nonint_dsml.raw.bin","wb").write(seg)
sep=b'\xef\xbd\x9c'
print("captured",len(seg),"bytes")
print("raw DSML open tag present:", b'<'+sep+b'DSML'+sep+b'tool_calls>' in seg)
print("fullwidth sep U+FF5C present:", sep in seg)
print("tool_result block present:", b'<tool_result>' in seg)
print("=== repr (first 900) ===")
print(repr(seg[:900]))
stop[0]=True; p.terminate()
try: p.wait(timeout=5)
except: p.kill()
