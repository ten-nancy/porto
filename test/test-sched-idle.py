from test_common import *
import porto

try:
    # Has extending SCHED_IDLE for cgroups https://lore.kernel.org/lkml/20210608231132.32012-1-joshdon@google.com/
    has_extended_sched_idle = os.path.exists('/sys/fs/cgroup/cpu/cpu.idle')
    cpu_idle = 'cpu.idle'
    policies = ["idle", "normal"]

    conn = porto.Connection(timeout=30)

    # Enable extending SCHED_IDLE for cgroups
    ConfigurePortod('test-sched-idle', """container {
        enable_sched_idle: true
    }""")

    if has_extended_sched_idle: # Linux with kernel 5.15 and above
        def convert_to_bool(p):
            return 1 if p == "idle" else 0

        # without cpu cgroup
        print("\nTest without cpu cgroup\na      b")
        for cp_a in policies:
            for cp_b in policies:
                print("{:7}{:7}".format(cp_a, cp_b))

                # note: level 1 containers always have cpu cgroup, so controllers property is not neccessary
                a = conn.Run("a", command='sleep inf', cpu_policy=cp_a, controllers="cpu", weak=True)
                ExpectEq(a[cpu_idle], "%d\n" % convert_to_bool(cp_a))

                b = conn.Run("a/b", command='sleep inf', cpu_policy=cp_b, weak=True)
                ExpectEq(b[cpu_idle], "%d\n" % convert_to_bool(cp_a))

                a.Destroy()

        # with cpu cgroup
        print("\nTest with cpu cgroup\na      b")
        for cp_a in policies:
            for cp_b in policies:
                print("{:7}{:7}".format(cp_a, cp_b))

                # note: level 1 containers always have cpu cgroup, so controllers property is not neccessary
                a = conn.Run("a", command='sleep inf', cpu_policy=cp_a, controllers="cpu", weak=True)
                ExpectEq(a[cpu_idle], "%d\n" % convert_to_bool(cp_a))

                b = conn.Run("a/b", command='sleep inf', cpu_policy=cp_b, controllers="cpu", weak=True)
                ExpectEq(b[cpu_idle], "%d\n" % convert_to_bool(cp_b))

                a.Destroy()

        # three containers
        print("\nTest three container\na      b      c")
        for cp_a in policies:
            for cp_b in policies:
                for cp_c in policies:
                    print("{:7}{:7}{:7}".format(cp_a, cp_b, cp_c))

                    # note: level 1 containers always have cpu cgroup, so controllers property is not neccessary
                    a = conn.Run("a", command='sleep inf', cpu_policy=cp_a, controllers="cpu", weak=True)
                    ExpectEq(a[cpu_idle], "%d\n" % convert_to_bool(cp_a))

                    # note: b without cpu cgroup
                    b = conn.Run("a/b", command='sleep inf', cpu_policy=cp_b, weak=True)
                    ExpectEq(b[cpu_idle], "%d\n" % convert_to_bool(cp_a))

                    c = conn.Run("a/b/c", command='sleep inf', cpu_policy=cp_c, controllers="cpu", weak=True)
                    ExpectEq(c[cpu_idle], "%d\n" % convert_to_bool(cp_c))

                    a.Destroy()

        ConfigurePortod('test-sched-idle', """container {
            enable_sched_idle: false
        }""")

        # without enable_sched_idle and with cpu cgroup
        print("\nTest without enable_sched_idle and with cpu cgroup\na      b")
        for cp_a in policies:
            for cp_b in policies:
                print("{:7}{:7}".format(cp_a, cp_b))

                # note: level 1 containers always have cpu cgroup, so controllers property is not neccessary
                a = conn.Run("a", command='sleep inf', cpu_policy=cp_a, controllers="cpu", weak=True)
                ExpectEq(a[cpu_idle], "0\n")

                b = conn.Run("a/b", command='sleep inf', cpu_policy=cp_b, controllers="cpu", weak=True)
                ExpectEq(b[cpu_idle], "0\n")

                a.Destroy()

    else:   # Linux kernel before 5.15

        # Linux on kernel before 5.15
        print("\nTest on Linux kernel before 5.15")

        # note: level 1 containers always have cpu cgroup, so controllers property is not neccessary
        a = conn.Run("a", command='sleep inf', cpu_policy="idle", controllers="cpu", weak=True)
        ExpectException(a.GetProperty, porto.exceptions.InvalidProperty, cpu_idle)
        a.Destroy()

finally:
    ConfigurePortod('test-sched-idle', "")
