import os,sys,time,subprocess,threading,select,re
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
env=dict(os.environ); env.update({"LINENOISE_ASSUME_TTY":"1","TERM":"dumb"})
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","8192","-n","160","--nothink","--seed","1"],
   cwd=DS4,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
out=bytearray(); stop=[False]; lock=threading.Lock(); seen=[0]
def raw(b):
    with lock: p.stdin.write(b); p.stdin.flush()
def pump():
    while not stop[0]:
        r,_,_=select.select([p.stdout,p.stderr],[],[],0.1)
        for f in r:
            b=os.read(f.fileno(),65536)
            if not b: continue
            if f is p.stdout:
                out.extend(b)
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
wait(b"ds4-agent>",600); time.sleep(0.5)
mark=len(out)
raw(b"Write a haiku about the ocean. Then on separate lines count: one, two, three.\r")
# wait for turn end: prompt reappears well after mark
time.sleep(1.0)
end=-1; t=time.time()
while time.time()-t<120:
    # turn ends when 'ds4-agent>' reappears after the generated content settles
    if bytes(out).rfind(b"ds4-agent>")>mark+50 and bytes(out).find(b"three",mark)>0:
        time.sleep(1.5); break
    time.sleep(0.3)
seg=bytes(out[mark:])
open("capture/stream.raw.bin","wb").write(seg)
print("captured",len(seg),"bytes of TUI generation -> capture/stream.raw.bin")
stop[0]=True
raw(b"/quit\r"); time.sleep(0.5)
try: p.terminate(); p.wait(timeout=5)
except: p.kill()
