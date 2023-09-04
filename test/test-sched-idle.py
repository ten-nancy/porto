from test_common import *
import porto

try:
    # Has extending SCHED_IDLE for cgroups https://lore.kernel.org/lkml/20210608231132.32012-1-joshdon@google.com/
    has_extended_sched_idle = os.path.exists('/sys/fs/cgroup/cpu/cpu.idle')

    c = porto.Connection(timeout=30)

    cpu_idle = 'cpu.idle'
    name = 'sched-idle'
    subname = 'sched-idle/sub'
    subsubname = 'sched-idle/sub/sub'

    # Enable extending SCHED_IDLE for cgroups
    ConfigurePortod('test-sched-idle', """container {
        enable_sched_idle: true
    }""")

    if has_extended_sched_idle: # Linux with kernel 5.15 and above

        # --- Test cpu_policy=idle
        ct = c.Run(name, cpu_policy='idle', weak=True)  # meta, idle
        ExpectEq(ct[cpu_idle], "1\n")
        subct = c.Run(subname, cpu_policy='idle', weak=True)  # meta, idle
        ExpectEq(subct[cpu_idle], "1\n")
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', command='echo', weak=True)  # none-meta, idle
        ExpectEq(subct[cpu_idle], "1\n")
        subct.Destroy()
        ExpectException(c.Run, porto.exceptions.InvalidValue, subname, cpu_policy='normal', weak=True)  # try meta normal in idle - forbidden
        ExpectException(c.Run, porto.exceptions.InvalidValue, subname, cpu_policy='normal', command='echo', weak=True)  # try none-meta normal in idle - forbidden
        ct.Destroy()

        # --- Test cpu_policy != idle
        ct = c.Run(name, cpu_policy='normal', weak=True)  # meta, normal
        ExpectEq(ct[cpu_idle], "0\n")
        subct = c.Run(subname, cpu_policy='normal', weak=True)  # meta, normal
        ExpectEq(subct[cpu_idle], "0\n")
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='normal', command='echo', weak=True)  # none-meta, normal
        ExpectEq(subct[cpu_idle], "0\n")
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', weak=True)  # meta, idle, has not CPU cgroup
        ExpectEq(subct[cpu_idle], "0\n")
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', command='echo', weak=True)  # none-meta, idle, has not CPU cgroup
        ExpectEq(subct[cpu_idle], "0\n")
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', controllers="cpu", weak=True)  # meta, idle, has CPU cgroup
        ExpectEq(subct[cpu_idle], "1\n")
        ExpectException(c.Run, porto.exceptions.InvalidValue, subsubname, cpu_policy='normal', weak=True)  # try meta normal in idle - forbidden
        ExpectException(c.Run, porto.exceptions.InvalidValue, subsubname, cpu_policy='normal', command='echo', weak=True)  # try none-meta normal in idle - forbidden
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', controllers="cpu", command='echo', weak=True)  # none-meta, idle, has CPU cgroup
        ExpectEq(subct[cpu_idle], "1\n")
        subct.Destroy()
        ct.Destroy()

        # Disable extending SCHED_IDLE for cgroups
        ConfigurePortod('test-sched-idle', """container {
            enable_sched_idle: false
        }""")

        # --- Test disabled for cpu_policy=idle
        ct = c.Run(name, cpu_policy='idle', weak=True)  # meta, idle
        ExpectEq(ct[cpu_idle], "0\n")
        subct = c.Run(subname, cpu_policy='idle', weak=True)  # meta, idle
        ExpectEq(subct[cpu_idle], "0\n")
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', command='echo', weak=True)  # none-meta, idle
        ExpectEq(subct[cpu_idle], "0\n")
        subct.Destroy()
        ct.Destroy()

        # --- Test enabled by default
        ConfigurePortod('test-sched-idle', "")
        ct = c.Run(name, cpu_policy='idle', weak=True)  # meta, idle
        ExpectEq(ct[cpu_idle], "1\n")
        subct = c.Run(subname, cpu_policy='idle', weak=True)  # meta, idle
        ExpectEq(subct[cpu_idle], "1\n")
        ExpectException(c.Run, porto.exceptions.InvalidValue, subsubname, cpu_policy='normal', weak=True)  # try meta normal in idle - forbidden
        ExpectException(c.Run, porto.exceptions.InvalidValue, subsubname, cpu_policy='normal', command='echo', weak=True)  # try none-meta normal in idle - forbidden
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', command='echo', weak=True)  # none-meta, idle
        ExpectEq(subct[cpu_idle], "1\n")
        subct.Destroy()
        ct.Destroy()

    else:   # None-Linux or kernel before 5.15

        # --- Test absent cpu.idle property
        ct = c.Run(name, cpu_policy='idle', weak=True)  # meta, idle
        ExpectException(ct.GetProperty, porto.exceptions.InvalidProperty, cpu_idle)
        subct = c.Run(subname, cpu_policy='idle', weak=True)  # meta, idle
        ExpectException(subct.GetProperty, porto.exceptions.InvalidProperty, cpu_idle)
        subct.Destroy()
        subct = c.Run(subname, cpu_policy='idle', command='echo', weak=True)  # none-meta, idle
        ExpectException(subct.GetProperty, porto.exceptions.InvalidProperty, cpu_idle)
        subct.Destroy()
        ct.Destroy()

finally:
    ConfigurePortod('test-sched-idle', "")
