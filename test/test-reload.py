import porto
from multiprocessing.pool import ThreadPool
import subprocess
from uuid import uuid4
import time
import random
import signal

def work():
    # time.sleep(random.random() * 16)
    conn = porto.Connection(timeout=30)
    cid = str(uuid4())

    spec = porto.rpc_pb2.TContainerSpec()
    spec.name = cid

    while True:
        with open("/run/portod.pid") as f:
            server_pid = int(f.read())
        spec.command = "kill -SIGHUP {}".format(server_pid)
        spec.isolate = False

        ct = conn.CreateSpec(container=spec, start=True)
        ct.Wait(timeout=30000)
        print(ct['exit_code'])
        print(ct['stdout'])
        print(ct['stderr'])

        ct.Destroy()
        time.sleep(random.random() * 0.01)


def main():
    work()
    tp = ThreadPool(processes=16)
    for _ in range(1):
        tp.apply_async(work)

    signal.pause()

#    while True:
#        subprocess.check_call(['portod', 'reload'])
#        time.sleep(random.random() * 0.1)


if __name__ == '__main__':
    main()
