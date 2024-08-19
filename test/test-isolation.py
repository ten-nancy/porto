import porto
from test_common import *

c = porto.Connection(timeout=30)

DefaultCapBnd = "0000003fffffffff"
PortoHostCapBnd = "00000000a9ec77fb"
if GetKernelVersion() >= (5, 15):
    DefaultCapBnd =  "000001ffffffffff"
    PortoHostCapBnd = "00000080a9ec77fb"

ExpectEq(ProcStatus('self', "CapBnd"), DefaultCapBnd)

# root user

a = c.Run("a", weak=True)
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), PortoHostCapBnd)
ExpectEq(ProcStatus(pid, "CapEff"), PortoHostCapBnd)
ExpectEq(ProcStatus(pid, "CapBnd"), PortoHostCapBnd)
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", virt_mode='host', command="sleep 10000", weak=True)
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), DefaultCapBnd)
ExpectEq(ProcStatus(pid, "CapEff"), DefaultCapBnd)
ExpectEq(ProcStatus(pid, "CapBnd"), DefaultCapBnd)
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

# non root user
AsAlice()

c = porto.Connection()

a = c.Run("a", weak=True)
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), PortoHostCapBnd)
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", virt_mode='host', command="sleep 10000", weak=True)
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), DefaultCapBnd)
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", isolate='false', root_volume={"layers": ["ubuntu-precise"]}, weak=True)
pid = a['root_pid']

ExpectEq(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), "00000000a80425db")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

a = c.Run("a", net='none', memory_limit='1G', root_volume={"layers": ["ubuntu-precise"]}, weak=True)
pid = a['root_pid']

ExpectNe(ProcStatus(pid, "NSpid"), pid)
ExpectEq(ProcStatus(pid, "CapInh"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapPrm"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapEff"), "0000000000000000")
ExpectEq(ProcStatus(pid, "CapBnd"), "00000000a80c75fb")
ExpectEq(ProcStatus(pid, "CapAmb"), "0000000000000000")

a.Destroy()

# virt_mode=host restrictions

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
