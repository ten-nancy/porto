#!/usr/bin/python3

import os
import porto
import subprocess
import time
import tempfile

from test_common import ConfigurePortod,Expect,ExpectException,ExpectLe

def veth_link_count():
    return len(subprocess.check_output(["ip", "-o", "link", "show", "type", "veth"]).split())

# Prepare

script_path = tempfile.NamedTemporaryFile().name

open(script_path,'w')
os.chmod(script_path, 0o755)

cwd = os.getcwd()
portoctl_path = cwd + '/portoctl'

ConfigurePortod('net-ifup',"""
network {
  network_ifup_script: \"%s\"
}
""" % script_path)

c = porto.Connection(timeout=30)
ct = c.CreateWeakContainer('test-ifup-script')
ct.SetProperty('net', "L3 veth")
ct.SetProperty('labels', 'AA.aaa: test')
ct.SetProperty('net_limit', 'default: 7255')
ct.SetProperty('net_rx_limit', 'default: 7255')


# 1. Check invocation & env

stdout_path = portoctl_path + '_log'

open(script_path, 'w').write("""
#!/bin/bash
env > %s
""" % (stdout_path))

ct.Start()
ct.Stop()

env = open(stdout_path, 'r').read()

Expect('PORTO_CONTAINER=test-ifup-script' in env)
Expect('PORTO_LABELS=AA.aaa: test' in env)
Expect('PORTO_NET=L3 veth' in env)
Expect('PORTO_IP=' in env)
Expect('PORTO_NET_LIMIT=default: 7255' in env)
Expect('PORTO_NET_RX_LIMIT=default: 7255' in env)
Expect('PORTO_L3_IFACE=L3-' in env)
Expect('PORTO_NETNS_FD=/proc' in env)

open(script_path, 'w').write("""
#!/bin/bash
exit 1
""")

stale_count = veth_link_count()

for i in range(10):
    ExpectException(ct.Start, porto.exceptions.Unknown)

# Nets are cleared asynchronously
deadline = time.time() + 30.0

while time.time() < deadline and veth_link_count() > stale_count:
    time.sleep(1)

ExpectLe(veth_link_count(), stale_count)


# 2. Check interaction with portod

open(script_path, 'w').write("""
#!/bin/bash
%s get / memory_usage > %s
""" % (portoctl_path, stdout_path))

ct.Start()
ct.Stop()

Expect(int(open(stdout_path, 'r').read()) > 0)


# 3. Check portod write permission

open(script_path, 'w').write("""
#!/bin/bash
%s create ifup-test
""" % (portoctl_path))

ExpectException(ct.Start, porto.exceptions.Unknown)

open(script_path, 'w').write("""
#!/bin/bash
%s set test-ifup-script cpu_policy idle
""" % (portoctl_path))

ExpectException(ct.Start, porto.exceptions.Unknown)

os.unlink(script_path)
os.unlink(stdout_path)

ConfigurePortod('net-ifup','')
