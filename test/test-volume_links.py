import test_common

import multiprocessing
import time
import porto
import os

CT_COUNT = 4
ITER_COUNT = 50
RUN_TIMEOUT = 180
STORAGE_NAME = "test-volume_lists-storage"

def init_instance(c, name, volume, target="/target", layers=["ubuntu-jammy"]):
    r = c.CreateWeakContainer(name)
    #r = c.Create(name)
    root = c.CreateVolume(layers=['ubuntu-jammy'])
    r.SetProperty("root", root)

    try:
        c.LinkVolume(volume, name, target="/target")
    except Exception as e:
        print(e)
        raise e

    return (r, root)


def proc_loop(name, volume, ctx, conn=porto.Connection()):
    while True:
        r, root = init_instance(conn, name, volume)
        root.Unlink()
        r.Destroy()

        ctx.Cv.acquire()
        ctx.Counter.value += 1
        if ctx.Counter == CT_COUNT - 1:
            ctx.Cv.notify_all()

        while ctx.Counter.value > 0:
            ctx.Cv.wait()
        ctx.Cv.release()


class Ctx(object):
    def __init__(self):
        self._lock = multiprocessing.Lock()
        self.Cv = multiprocessing.Condition(self._lock)
        self.Counter = multiprocessing.Value('i', 0)


c = porto.Connection(timeout=30)

share = c.CreateVolume(None, **{"storage":STORAGE_NAME, "space_limit":"5G"})

main_ct, root = init_instance(c, "test-volume_links-ct{}".format(CT_COUNT), share.path)

print("mount count before: {}".format(len(open('/proc/self/mountinfo').readlines())))

main_ct.Start()

ctx = Ctx()

workers = [
    multiprocessing.Process(target=proc_loop,
        args=(
            "test-volume_links-ct{}".format(i),
            share.path,
            ctx
        )
    ) for i in range(1, CT_COUNT)
]

for w in workers:
    w.start()

dead = False
timeout = False

deadline = time.time() + RUN_TIMEOUT

for i in range(ITER_COUNT):
    ctx.Cv.acquire()
    while ctx.Counter.value < CT_COUNT - 1:
        ctx.Cv.release()
        for w in workers:
            w.join(0.01)
            if not w.is_alive():
                dead = True

        if dead:
            break

        if time.time() > deadline:
            timeout = True
            break

        ctx.Cv.acquire()

    if dead or timeout:
        break

    ctx.Counter.value = 0
    ctx.Cv.notify_all()
    ctx.Cv.release()

exec_ct = c.Create(main_ct.name + "/findmnt")
exec_ct.SetProperty("command", 'cat /proc/self/mountinfo')
exec_ct.Start()
exec_ct.Wait()

print("mount count after: {}".format(len(open('/proc/self/mountinfo').readlines())))

mounts = exec_ct.GetProperty("stdout").splitlines()

targets = [line for line in mounts if "/target" in line]

print("\n".join(targets))

timeout = time.time() > deadline

for w in workers:
    w.terminate()

main_ct.Destroy()
share.Unlink()
root.Unlink()
c.RemoveStorage(STORAGE_NAME)

for v in c.ListVolumes():
    v.Unlink()

assert len(c.ListVolumes()) == 0

assert len(targets) == 1, "Container shared mount count: {} exceeded one".format(len(targets))
assert not dead, "Worker has dead prematurely"
assert not timeout, "Test timed out"

# "Try to link and unlink volumes with targets"

link_path = "/tmp/porto/selftest/a/link"
sublink_path = "/tmp/porto/selftest/a/link/sublink"
sub_sublink_path = "/tmp/porto/selftest/a/link/sublink/subsublink/abc"

v1 = c.CreateVolume()
v2 = c.CreateVolume()
v3 = c.CreateVolume()

if not os.path.exists(sub_sublink_path):
    os.makedirs(sub_sublink_path)

c.LinkVolume(v1.path, 'self', link_path)
c.LinkVolume(v3.path, 'self', sublink_path)

c.UnlinkVolume(link_path) # must unlink sublink and destroy v1

c.LinkVolume(v3.path, 'self', sublink_path) # sublink must be free
c.LinkVolume(v2.path, 'self', sub_sublink_path)

c.UnlinkVolume(sublink_path) # must unlink sub_sublink and destroy v3

c.LinkVolume(v2.path, 'self', sub_sublink_path) # it must be free

c.UnlinkVolume(v2.path)

assert len(c.ListVolumes()) == 0
