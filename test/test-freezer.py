from test_common import *
import porto
import subprocess
import os

conn = porto.Connection(timeout=30)

PORTO_FREEZER_CGROUP_ROOT = "/sys/fs/cgroup/freezer/porto"
SLEEP_DURATION = 120

try:
    for h in ("b", "b/c", "b/c/d"):
        a = conn.Run("a", command="sleep %u" % SLEEP_DURATION, weak=True)
        cgroup_a = a.GetProperty("cgroups[freezer]")
        cgroup_path = os.path.join(cgroup_a, h)

        print("Test", cgroup_path)
        os.makedirs(cgroup_path)

        process = subprocess.Popen("sleep %u &" % SLEEP_DURATION, shell=True)
        with open("%s/cgroup.procs" % cgroup_path, "w") as f:
            f.write(str(process.pid))

        with open("%s/freezer.state" % cgroup_path, "w") as f:
            f.write("FROZEN")

        a.Destroy()

        Expect(not os.path.exists(cgroup_a))

finally:
    for root, dirs, _ in os.walk(PORTO_FREEZER_CGROUP_ROOT):
        for name in dirs:
            with open(os.path.join(root, name, "freezer.state"), "w") as f:
                f.write("THAWED")
