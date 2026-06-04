import os,time,subprocess,threading,select
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
env=dict(os.environ); env.update({"LINENOISE_ASSUME_TTY":"1","TERM":"dumb"})
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","8192","-n","220","--nothink","--seed","3"],
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
        i=bytes(out).find(sub,after)
        if i>=0: return i
        time.sleep(0.1)
    return -1
wait(b"ds4-agent>",600); time.sleep(0.5); mark=len(out)
raw(b"Create a file at /tmp/pinback_probe.txt containing exactly the word banana. Use your file tool.\r")
# wait until a DSML tool_calls block appears in raw stream, then settle
dsml=b"DSML"  # 0xEF 0xBD 0x9C handling: just look for tool_calls marker bytes
t=time.time(); got=False
while time.time()-t<150:
    if bytes(out).find(b"tool_calls>",mark)>0: got=True; time.sleep(4.0); break
    time.sleep(0.3)
open("capture/dsml.raw.bin","wb").write(bytes(out[mark:]))
print("captured",len(out)-mark,"bytes; tool_calls seen:",got)
stop[0]=True; raw(b"/quit\r"); time.sleep(0.5)
try: p.terminate(); p.wait(timeout=5)
except: p.kill()
