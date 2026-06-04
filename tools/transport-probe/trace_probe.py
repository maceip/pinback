import os,time,subprocess,threading,select
DS4=os.path.expanduser("~/ds4"); AGENT=DS4+"/ds4-agent"; MODEL=DS4+"/ds4flash.gguf"
TRACE="/tmp/ds4_trace_probe.log"
try: os.remove(TRACE)
except OSError: pass
p=subprocess.Popen([AGENT,"--model",MODEL,"-c","8192","-n","256","--nothink","--seed","5",
                    "--non-interactive","--trace",TRACE],
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
# A prompt that forces an edit (write) AND a bash command, so the trace shows both.
p.stdin.write(b"Do two things: (1) create file /tmp/trace_probe.txt containing the word mango using your write tool; (2) run the bash command 'echo hello-from-bash'. Then say done.\n"); p.stdin.flush()
waitn(n+1,200)
open("capture/nonint_trace_stdout.bin","wb").write(bytes(out))
stop[0]=True; p.terminate()
try: p.wait(timeout=5)
except: p.kill()
print("STDOUT bytes:",len(out))
print("trace exists:",os.path.exists(TRACE),"size:",os.path.getsize(TRACE) if os.path.exists(TRACE) else 0)
