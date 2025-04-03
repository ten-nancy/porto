import porto
from test_common import *

c = porto.Connection(timeout=30)

AllCapSet           = "0000003fffffffff"
DefaultCapSet       = "00000000a80c75fb" # DefaultCapabilities
HardlyClearedCapSet = "00000000a80c25fb" # DefaultCapabilities & ~(CAP_NET_ADMIN | CAP_IPC_LOCK)
FullyClearedCapSet  = "00000000a80425db" # DefaultCapabilities & ~(CAP_NET_ADMIN | CAP_IPC_LOCK | CAP_KILL | CAP_SYS_PTRACE)
RootCapSet          = "00000000a9ec77fb" # DefaultCapabilities | CAP_SYS_ADMIN | CAP_SYS_NICE | CAP_LINUX_IMMUTABLE | CAP_SYS_BOOT | CAP_SYS_RESOURCE
EmptyCapSet         = "0000000000000000"
CustomCapSet        = "0000000000000180" # SETUID | SETPCAP
if GetKernelVersion() >= (5, 15):
    AllCapSet       =  "000001ffffffffff"

ExpectEq(ProcStatus('self', "CapBnd"), AllCapSet)

# root user

a = c.Run("a", weak=True)
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), RootCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), RootCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), RootCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

a.Destroy()

a = c.Run("a", virt_mode='host', command="sleep 10000", weak=True)
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), AllCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), AllCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), AllCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

a.Destroy()

# non root user
AsAlice()

c = porto.Connection(timeout=30)

a = c.Run("a", weak=True)
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), HardlyClearedCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

a.Destroy()

a = c.Run("a", virt_mode='host', command="sleep 10000", weak=True)
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), AllCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

a.Destroy()

a = c.Run("a", isolate='false', root_volume={"layers": ["ubuntu-jammy"]}, weak=True)
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), FullyClearedCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

a.Destroy()

a = c.Run("a", net='none', memory_limit='1G', root_volume={"layers": ["ubuntu-jammy"]}, weak=True)
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), DefaultCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

a.Destroy()

# virt_mode=host restrictions

a = c.Run("a", virt_mode='host', command="sleep inf", weak=False)
pid = a['root_pid']

ExpectEq(len(a.GetProperty("capabilities")), 151)
ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), AllCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

AsRoot()
ReloadPortod()
AsAlice()

ExpectEq(a['state'], "running")
a.Destroy()

a = c.Run("a", virt_mode='host', capabilities="SETUID;SETPCAP", command="sleep inf", weak=False)
pid = a['root_pid']

ExpectEq(a.GetProperty("capabilities"), "SETUID;SETPCAP")
ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapPrm"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapEff"), EmptyCapSet)
ExpectEq(ProcStatus(pid, "CapBnd"), CustomCapSet)
ExpectEq(ProcStatus(pid, "CapAmb"), EmptyCapSet)

AsRoot()
ReloadPortod()
AsAlice()

ExpectEq(a['state'], "running")
a.Destroy()

a = c.Run("a")
ExpectEq(Catch(c.Run, "a/b", virt_mode='host', weak=True), porto.exceptions.Permission)
a.Destroy()

# the order of virt_mode and isolate set may impact to test result, so set property explicitely
a = c.Create("a", weak=True)
a.SetProperty('virt_mode', 'host')
ExpectEq(Catch(a.SetProperty, 'isolate', 'true'), porto.exceptions.InvalidValue)
a.Destroy()

ExpectEq(Catch(c.Run, "a", virt_mode='host', root="/a", weak=True), porto.exceptions.InvalidValue)
ExpectEq(Catch(c.Run, "a", virt_mode='host', bind="/a /b", weak=True), porto.exceptions.InvalidValue)
