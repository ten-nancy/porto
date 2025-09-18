from test_common import *

import sys
import os
import porto
import random
import time
import hashlib
from traceback import print_exc

ConfigurePortod('test-spec', """
core {
    enable: true
}""")

AsAlice()

c = porto.Connection(timeout=30)

c.connect()

prefix = "test-spec-"
container_name_a = prefix + "a"
container_name_b = prefix + "b"

def CopyProps(ct_a, ct_b):
    spec = ct_a.Dump().spec
    spec.env_secret.Clear()
    ct_b.LoadSpec(ct_a.Dump().spec)

def CheckVolatileProp(v1, v2):
    assert v1*0.95 <= v2 <= v1*1.05

ab = a = b = None
try:
    a = c.Create(container_name_a, weak=True)
    b = c.Create(container_name_b, weak=True)

    # check env

    secret_value = '/porto/src/api/python'

    a.SetProperty('env', 'PYTHONPATH1=/porto/src/api/python')
    a.SetProperty('env_secret', 'PYTHONPATH_SECRET=' + secret_value)
    assert a.GetProperty('env[PYTHONPATH1]') ==  '/porto/src/api/python'
    assert a.GetProperty('env_secret[PYTHONPATH_SECRET]').startswith('<secret salt=')
    assert a.GetProperty('env_secret[PYTHONPATH_SECRET]') != a.GetProperty('env_secret[PYTHONPATH_SECRET]')

    dump = a.Dump()

    envs = {}
    for env in dump.spec.env.var:
        envs[env.name] = env.value
    assert envs['PYTHONPATH1'] == a.GetProperty('env[PYTHONPATH1]')
    assert envs['USER'] == a.GetProperty('env[USER]')

    assert len(dump.spec.env_secret.var) == 1
    for secret in dump.spec.env_secret.var:
        assert secret.value == '<secret>'
        assert hashlib.md5(str(secret.salt + secret_value).encode()).hexdigest() == secret.hash

    CopyProps(a, b)

    envs = {}
    for env in b.Dump().spec.env.var:
        envs[env.name] = env.value
    assert envs['PYTHONPATH1'] == a.GetProperty('env[PYTHONPATH1]')
    assert envs['USER'] == a.GetProperty('env[USER]')

    assert dump.spec.name == container_name_a
    assert c.Find(container_name_a).name == container_name_a
    assert container_name_a in c.List()

    # check caps

    caps = a.GetProperty('capabilities')
    dump_caps = []

    for cap in dump.spec.capabilities.cap:
        dump_caps.append(cap)

    assert ';'.join(dump_caps) == caps

    dump_caps_ambient = []
    for cap in dump.spec.capabilities_ambient.cap:
        dump_caps_ambient.append(cap)

    assert ';'.join(dump_caps_ambient) == a.GetProperty('capabilities_ambient')

    b_spec = porto.rpc_pb2.TContainerSpec()
    b_spec.CopyFrom(dump.spec)
    assert a.GetProperty('capabilities') == b.GetProperty('capabilities')

    caps_allowed = []
    for cap in dump.status.capabilities_allowed.cap:
        caps_allowed.append(cap)

    assert ';'.join(caps_allowed) == a.GetProperty('capabilities_allowed')

    caps_ambiend_allowed = []
    for cap in dump.status.capabilities_ambient_allowed.cap:
        caps_ambiend_allowed.append(cap)

    assert ';'.join(caps_ambiend_allowed) == a.GetProperty('capabilities_ambient_allowed')


    assert dump.spec.cwd == a.GetProperty('cwd')


    a.SetProperty('ulimit', 'core: 999')
    CopyProps(a, b)

    assert a.GetProperty('ulimit[core]') == b.GetProperty('ulimit[core]')

    dump = a.Dump()

    for ulimit in dump.spec.ulimit.ulimit:
        if ulimit.type == 'core':
            ulimit_prop = a.GetProperty('ulimit[core]').split(' ')
            assert ulimit.soft == int(ulimit_prop[1])
            assert ulimit.hard == int(ulimit_prop[2])


    a.SetProperty('cpu_policy', 'high')
    CopyProps(a, b)
    dump = a.Dump()
    assert dump.spec.cpu_policy == a.GetProperty('cpu_policy')
    assert a.GetProperty('cpu_policy') == b.GetProperty('cpu_policy')

    a.SetProperty('io_policy', 'normal')
    CopyProps(a, b)
    dump = a.Dump()
    assert dump.spec.io_policy == a.GetProperty('io_policy')
    assert a.GetProperty('io_policy') == b.GetProperty('io_policy')

    a.SetProperty('io_weight', '87.5')
    CopyProps(a, b)
    dump = a.Dump()
    assert float(a.GetProperty('io_weight')) == dump.spec.io_weight
    assert a.GetProperty('io_weight') == b.GetProperty('io_weight')

    assert a.GetProperty('task_cred') == b.GetProperty('task_cred')
    task_cred = [int(i) for i in a.GetProperty('task_cred').split()]
    assert dump.spec.task_cred.uid == task_cred[0]
    assert dump.spec.task_cred.gid == task_cred[1]
    assert dump.spec.task_cred.grp[0] == task_cred[2]
    assert dump.spec.task_cred.grp[1] == task_cred[3]

    assert dump.spec.user == a.GetProperty('user')
    assert a.GetProperty('user') == b.GetProperty('user')

    assert dump.spec.group == a.GetProperty('group')

    assert dump.spec.owner_cred.user == a.GetProperty('owner_user')
    assert dump.spec.owner_cred.group == a.GetProperty('owner_group')

    assert dump.spec.owner_user == a.GetProperty('owner_user')
    assert dump.spec.owner_group == a.GetProperty('owner_group')

    a.SetProperty("memory_guarantee", "2M")
    CopyProps(a, b)
    assert a.GetProperty('memory_guarantee') == b.GetProperty('memory_guarantee')
    dump = a.Dump()
    assert dump.spec.memory_guarantee == int(a.GetProperty('memory_guarantee'))

    assert dump.status.memory_guarantee_total == int(a.GetProperty('memory_guarantee_total'))

    assert dump.status.state == a.GetProperty('state')

    a.SetProperty("command_argv", "sleep\t321")
    assert a.GetProperty("command_argv") == "sleep\t321"
    CopyProps(a, b)
    assert b.GetProperty("command_argv") == "sleep\t321"

    a.SetProperty("command", "sleep 123")
    CopyProps(a, b)
    assert a.GetProperty('command') == b.GetProperty('command')

    dump = a.Dump()
    assert dump.spec.command == a.GetProperty('command')

    a.SetProperty('core_command', 'sleep 531')
    CopyProps(a, b)
    assert a.GetProperty('core_command') == b.GetProperty('core_command')

    dump = a.Dump()
    assert dump.spec.core_command == a.GetProperty('core_command')

    a.SetProperty('virt_mode', 'host')
    CopyProps(a, b)
    dump = a.Dump()
    assert a.GetProperty('virt_mode') == b.GetProperty('virt_mode')
    assert dump.spec.virt_mode == a.GetProperty('virt_mode')
    a.SetProperty('virt_mode', 'app')
    CopyProps(a, b)

    assert dump.spec.stdin_path == a.GetProperty('stdin_path')
    assert dump.spec.stdout_path == a.GetProperty('stdout_path')
    assert dump.spec.stderr_path == a.GetProperty('stderr_path')
    assert a.GetProperty('stdin_path') == b.GetProperty('stdin_path')
    assert a.GetProperty('stdout_path') == b.GetProperty('stdout_path')
    assert a.GetProperty('stderr_path') == b.GetProperty('stderr_path')

    assert dump.spec.stdout_limit == int(a.GetProperty('stdout_limit'))

    a.Start()
    assert dump.status.stdout_offset == int(a.GetProperty('stdout_offset'))
    assert dump.status.stderr_offset == int(a.GetProperty('stderr_offset'))
    a.Stop()

    a.SetProperty('isolate', 'true')
    CopyProps(a, b)
    dump = a.Dump()
    assert dump.spec.isolate == bool(a.GetProperty('isolate'))

    assert dump.spec.root == a.GetProperty('root')

    a.SetProperty('root', '/place')
    dump = a.Dump()
    CopyProps(a, b)

    assert a.GetProperty('root') == b.GetProperty('root')

    assert dump.status.root_path == a.GetProperty('root_path')
    assert a.GetProperty('root_path') == b.GetProperty('root_path')

    if a.GetProperty('net') == 'inherited':
        assert dump.spec.net.inherited == True

    assert a.GetProperty('net') == b.GetProperty('net')

    a.SetProperty('root_readonly', 'true')
    dump = a.Dump()
    CopyProps(a, b)
    assert dump.spec.root_readonly == a.GetProperty('root_readonly')
    assert a.GetProperty('root_readonly') == b.GetProperty('root_readonly')


    a.SetProperty('umask', '51')
    dump = a.Dump()
    CopyProps(a, b)
    assert dump.spec.umask == int(a.GetProperty('umask'))
    assert int(a.GetProperty('umask')) == int(b.GetProperty('umask'))

    controllers = []
    for controller in dump.spec.controllers.controller:
        controllers.append(controller)
    assert ';'.join(controllers) == a.GetProperty('controllers')

    cgroups = a.GetProperty('cgroups')
    for cgroup in dump.status.cgroups.cgroup:
        assert cgroups.find("{}: {}".format(cgroup.controller, cgroup.path)) != -1

    assert cgroups.count(';') + 1 == len(dump.status.cgroups.cgroup)

    assert dump.spec.hostname == a.GetProperty('hostname')

    a.SetProperty('bind', '/ /place123')
    binds = a.GetProperty('bind')
    CopyProps(a, b)
    assert a.GetProperty('bind') == b.GetProperty('bind')
    dump = a.Dump()
    for bind in dump.spec.bind.bind:
        assert binds.find('{} {}'.format(bind.source, bind.target)) != -1
    assert binds.count(';') + 1 == len(dump.spec.bind.bind)

    a.SetProperty('bind', '')

    a.SetProperty('symlink', '/:/place123')
    CopyProps(a, b)
    assert a.GetProperty('symlink') == b.GetProperty('symlink')
    symlinks =  a.GetProperty('symlink')
    dump = a.Dump()
    for symlink in dump.spec.symlink.map:
        assert symlinks.find('{}: {}'.format(symlink.key, symlink.val)) != -1
    assert symlinks.count(';') == len(dump.spec.symlink.map)
    a.SetProperty('symlink', '')


    a.SetProperty('ip', "veth 198.51.100.0")
    a.SetProperty('default_gw', "veth 198.51.100.1")
    CopyProps(a, b)
    assert a.GetProperty('ip') == b.GetProperty('ip')
    assert a.GetProperty('default_gw') == b.GetProperty('default_gw')

    dump = a.Dump()

    ips = a.GetProperty('ip')
    for ip in dump.spec.ip.cfg:
        assert ips.find('{} {}'.format(ip.dev, ip.ip)) != -1
    assert ips.count(';') + 1 == len(dump.spec.ip.cfg)

    assert dump.spec.ip_limit.policy == 'any'

    default_gw = a.GetProperty('default_gw')
    for gw in dump.spec.default_gw.cfg:
        assert default_gw.find("{} {}".format(gw.dev, gw.ip)) != -1
    assert default_gw.count(';') + 1 == len(dump.spec.default_gw.cfg)

    a.SetProperty('resolv_conf', "nameserver 1.1.1.1")
    CopyProps(a, b)
    assert a.GetProperty('resolv_conf') == b.GetProperty('resolv_conf')
    dump = a.Dump()

    assert dump.spec.resolv_conf == a.GetProperty('resolv_conf')

    assert dump.spec.etc_hosts == a.GetProperty('etc_hosts')
    assert a.GetProperty('etc_hosts') == b.GetProperty('etc_hosts')

    a.SetProperty('devices', '/dev/null rwm; /dev/zero r')
    CopyProps(a, b)
    assert a.GetProperty('devices') == b.GetProperty('devices')
    dump = a.Dump()

    devices =  a.GetProperty('devices')
    for device in dump.spec.devices.device:
        assert devices.find('{} {}'.format(device.access, device.device)) != -1
    assert devices.count(';') == len(dump.spec.devices.device)

    assert dump.spec.porto_namespace == a.GetProperty('porto_namespace')

    places = a.GetProperty('place')
    for place in dump.spec.place.cfg:
        assert places.find(place.place) != -1
    assert places.count(';') + 1 == len(dump.spec.place.cfg)

    v1 = c.CreateVolume(space_limit='400M', owner_container=container_name_a)
    v2 = c.CreateVolume(space_limit='500M', owner_container=container_name_a)
    dump = a.Dump()

    volumes_owned = a.GetProperty('volumes_owned')
    for volume_owned in dump.status.volumes_owned.volume:
        assert volumes_owned.find(volume_owned) != -1
    assert volumes_owned.count(';') + 1 == len(dump.status.volumes_owned.volume)

    volumes_linked = a.GetProperty('volumes_linked')
    for volume_linked in dump.status.volumes_linked.link:
        assert volumes_linked.find(volume_linked) != -1
    assert (0 if len(volumes_linked) == 0 else (volumes_linked.count(';') + 1)) == len(dump.status.volumes_linked.link)

    volumes_required = a.GetProperty('volumes_required')
    for volume_requireq in dump.spec.volumes_required.volume:
        assert volumes_linked.find(volumes_owned) != -1
    assert (0 if len(volumes_required) ==0 else (volumes_required.count(';') + 1)) == len(dump.spec.volumes_required.volume)

    v1.Destroy()
    v2.Destroy()

    a.SetProperty('memory_limit', '10G')
    a.SetProperty('anon_limit', '5G')
    a.SetProperty('dirty_limit', '4G')
    a.SetProperty('hugetlb_limit', '3G')

    CopyProps(a, b)
    dump = a.Dump()
    assert a.GetProperty('memory_limit') == b.GetProperty('memory_limit')
    assert a.GetProperty('memory_limit_total') == b.GetProperty('memory_limit_total')
    assert a.GetProperty('anon_limit') == b.GetProperty('anon_limit')
    assert a.GetProperty('anon_limit_total') == b.GetProperty('anon_limit_total')
    assert a.GetProperty('dirty_limit') == b.GetProperty('dirty_limit')
    assert a.GetProperty('hugetlb_limit') == b.GetProperty('hugetlb_limit')


    assert dump.spec.memory_limit == int(a.GetProperty('memory_limit'))

    assert dump.status.memory_limit_total == int(a.GetProperty('memory_limit_total'))

    assert dump.spec.anon_limit == int(a.GetProperty('anon_limit'))

    assert dump.status.anon_limit_total == int(a.GetProperty('anon_limit_total'))

    assert dump.spec.dirty_limit == int(a.GetProperty('dirty_limit'))

    assert dump.spec.hugetlb_limit == int(a.GetProperty('hugetlb_limit'))

    a.SetProperty('recharge_on_pgfault', 'true')
    a.SetProperty('pressurize_on_death', 'true')
    CopyProps(a, b)
    dump = a.Dump()

    ExpectEq(a.GetProperty('recharge_on_pgfault'), b.GetProperty('recharge_on_pgfault'))
    ExpectEq(a.GetProperty('pressurize_on_death'), b.GetProperty('pressurize_on_death'))

    ExpectEq(dump.spec.recharge_on_pgfault, int(a.GetProperty('recharge_on_pgfault')))

    ExpectEq(dump.spec.pressurize_on_death, bool(a.GetProperty('pressurize_on_death')))

    def gen_cpu_limit(order):
        sec = random.randint(0, 1)
        nsec = random.randint(0, 10**order - 1)
        if nsec == 0:
            return '{}c'.format(sec)
        nsec = '{:09}'.format(nsec)
        while nsec[-1] == '0':
            nsec = nsec[:-1]
        return '{}.{}c'.format(sec, nsec)

    for order in range(1, 10):
        for _ in range(100):
            cpu_limit = gen_cpu_limit(order)

            a.SetProperty("cpu_limit", cpu_limit)
            ExpectEq(a.GetProperty("cpu_limit"), cpu_limit)

    a.SetProperty("cpu_limit", "0.1c")
    ExpectEq(a.GetProperty("cpu_limit"), "0.1c")

    a.SetProperty("cpu_limit", "0.01c")
    ExpectEq(a.GetProperty("cpu_limit"), "0.01c")

    a.SetProperty("cpu_limit", "0.10c")
    ExpectEq(a.GetProperty("cpu_limit"), "0.1c")

    a.SetProperty("cpu_limit", "0.010c")
    ExpectEq(a.GetProperty("cpu_limit"), "0.01c")

    a.SetProperty("cpu_limit", "19.0947103c")
    ExpectEq(a.GetProperty("cpu_limit"), "19.0947103c")

    # TODO(ovov): 0.08255387071c is bad, remove this after enabling validation in porto
    a.SetProperty("cpu_limit", "0.08255387071c")
    ExpectEq(a.GetProperty("cpu_limit"), "0.08255387c")

    a.SetProperty("cpu_limit", "1.5c")
    a.SetProperty("cpu_guarantee", "0.5c")
    dump = a.Dump()
    CopyProps(a, b)
    ExpectEq(a.GetProperty('cpu_limit'), b.GetProperty('cpu_limit'))
    ExpectEq(a.GetProperty('cpu_limit'), '{}c'.format(dump.spec.cpu_limit))

# assert a.GetProperty('cpu_limit_total') == '{}c'.format(dump.spec.cpu_limit_total)

    ExpectEq(a.GetProperty('cpu_guarantee'), '{}c'.format(dump.spec.cpu_guarantee))
    ExpectEq(a.GetProperty('cpu_guarantee'), b.GetProperty('cpu_guarantee'))

    ExpectEq(float(a.GetProperty('cpu_guarantee_total')[:-1]), dump.status.cpu_guarantee_total)

    a.SetProperty('cpu_period', '1054000')
    a.SetProperty('cpu_weight', '41.5')
    dump = a.Dump()
    CopyProps(a, b)

    ExpectEq(a.GetProperty('cpu_period'), b.GetProperty('cpu_period'))
    ExpectEq(a.GetProperty('cpu_weight'), b.GetProperty('cpu_weight'))

    ExpectEq(dump.spec.cpu_period, int(a.GetProperty('cpu_period')))

    ExpectEq(dump.spec.cpu_weight, float(a.GetProperty('cpu_weight')))

    a.SetProperty('cpu_set', '0-1,3')
    dump = a.Dump()
    CopyProps(a, b)

    ExpectEq(a.GetProperty('cpu_set'), b.GetProperty('cpu_set'))

    ExpectEq(dump.spec.cpu_set.count, 3)
    ExpectEq(set(dump.spec.cpu_set.cpu), {0, 1, 3})

    ExpectEq(a.GetProperty('cpu_set_affinity'), '')
    a['root'] = '/'
    a.Start()
    ExpectEq(a.GetProperty('cpu_set'), a.GetProperty('cpu_set_affinity'))
    ExpectEq(dump.spec.cpu_set.count, 3)
    ExpectEq(set(dump.spec.cpu_set.cpu), {0, 1, 3})
    a.Stop()
    a['root'] = '/place'

    a.SetProperty("io_limit", "/ssd: 1000; /place: 500")
    a.SetProperty("io_guarantee", "/ssd: 1001; /place: 501")
    a.SetProperty("io_ops_limit", "/ssd: 1500; /place: 550")
    a.SetProperty("io_ops_guarantee", "/ssd: 1501; /place: 551")
    dump = a.Dump()
    CopyProps(a, b)

    assert a.GetProperty('io_limit') == b.GetProperty('io_limit')
    assert a.GetProperty('io_guarantee') == b.GetProperty('io_guarantee')
    assert a.GetProperty('io_ops_limit') == b.GetProperty('io_ops_limit')
    assert a.GetProperty('io_ops_guarantee') == b.GetProperty('io_ops_guarantee')

    io_limits = a.GetProperty('io_limit')
    for kv in dump.spec.io_limit.map:
        assert io_limits.find('{}: {}'.format(kv.key, kv.val)) != -1
    assert io_limits.count(';') + 1 == len(dump.spec.io_limit.map)

    io_guarantees = a.GetProperty('io_guarantee')
    for kv in dump.spec.io_guarantee.map:
        assert io_guarantees.find('{}: {}'.format(kv.key, kv.val)) != -1
    assert io_guarantees.count(';') + 1 == len(dump.spec.io_guarantee.map)

    io_ops_limits = a.GetProperty('io_ops_limit')
    for kv in dump.spec.io_ops_limit.map:
        assert io_ops_limits.find('{}: {}'.format(kv.key, kv.val)) != -1
    assert io_ops_limits.count(';') + 1 == len(dump.spec.io_ops_limit.map)

    io_ops_guarantees = a.GetProperty('io_ops_guarantee')
    for kv in dump.spec.io_ops_guarantee.map:
        assert io_ops_guarantees.find('{}: {}'.format(kv.key, kv.val)) != -1
    assert io_ops_guarantees.count(';') + 1 == len(dump.spec.io_ops_guarantee.map)

    a.SetProperty('max_respawns', '10')
    a.SetProperty('respawn_delay', '1500ns')
    dump = a.Dump()
    CopyProps(a, b)

    assert a.GetProperty('respawn_count') == b.GetProperty('respawn_count')
    assert a.GetProperty('respawn_delay') == b.GetProperty('respawn_delay')

    assert dump.spec.respawn == bool(a.GetProperty('respawn'))
    assert dump.spec.respawn_count == int(a.GetProperty('respawn_count'))
    assert dump.spec.max_respawns == int(a.GetProperty('max_respawns'))

    assert '{}ns'.format(dump.spec.respawn_delay) == a.GetProperty('respawn_delay')

    a.SetProperty('private', 'True')
    CopyProps(a, b)
    dump = a.Dump()

    assert a.GetProperty('private')  == b.GetProperty('private')
    assert dump.spec.private == a.GetProperty('private')

    a.SetProperty('labels', 'AGENT.val:521; AGENT.val2:123')
    dump = a.Dump()
    CopyProps(a, b)

    assert a.GetProperty('labels') == b.GetProperty('labels')

    labels = a.GetProperty('labels')
    for label in dump.spec.labels.map:
        assert labels.find('{}: {}'.format(label.key, label.val)) != -1
    assert labels.count(';') + 1 == len(dump.spec.labels.map)

    assert dump.spec.aging_time == int(a.GetProperty('aging_time'))

    a.SetProperty('enable_porto', 'isolate')
    dump = a.Dump()
    CopyProps(a, b)


    assert a.GetProperty('enable_porto') == b.GetProperty('enable_porto')
    assert a.GetProperty('enable_porto') == dump.spec.enable_porto

    assert dump.spec.weak == bool(a.GetProperty('weak'))

    assert dump.status.id == int(a.GetProperty('id'))

    assert dump.status.level == int(a.GetProperty('level'))

    assert dump.status.absolute_name == a.GetProperty('absolute_name')

    assert dump.status.absolute_namespace == a.GetProperty('absolute_namespace')

    assert dump.status.state == "stopped"
    assert dump.status.state == a.GetProperty('state')

    a.Destroy()

    a = c.Create(container_name_a)
    a.Start()

    ab = c.Run(a.name + '/b', wait=0, weak=True, command="python3 -c 'import time; a = [0 for i in range(1024 * 1024)]; time.sleep(3)'")
    time.sleep(1)

    dump = a.Dump()

    CheckVolatileProp(dump.status.memory_usage, int(a.GetProperty('memory_usage')))
    CheckVolatileProp(dump.status.memory_reclaimed, int(a.GetProperty('memory_reclaimed')))
    CheckVolatileProp(dump.status.anon_usage, int(a.GetProperty('anon_usage')))
    CheckVolatileProp(dump.spec.anon_max_usage, int(a.GetProperty('anon_max_usage')))
    CheckVolatileProp(dump.status.cache_usage, int(a.GetProperty('cache_usage')))
    CheckVolatileProp(dump.status.shmem_usage, int(a.GetProperty('shmem_usage')))
    CheckVolatileProp(dump.status.mlock_usage, int(a.GetProperty('mlock_usage')))
    CheckVolatileProp(dump.status.hugetlb_usage, int(a.GetProperty('hugetlb_usage')))

    ab.WaitContainer(5)
    ExpectEq(ab['exit_code'], '0')
    ab.Destroy()

    dump = a.Dump()
    assert int(a.GetProperty('minor_faults')) == dump.status.minor_faults
    assert int(a.GetProperty('major_faults')) == dump.status.major_faults

    vms = a.GetProperty('virtual_memory')
    for vals in vms.split(';'):
        val = vals.split(':')
        assert eval('dump.status.virtual_memory.{}'.format(val[0])) == int(val[1])

    assert int(a.GetProperty('cpu_usage')) == dump.status.cpu_usage
    assert int(a.GetProperty('cpu_usage_system')) == dump.status.cpu_usage_system
    assert int(a.GetProperty('cpu_wait')) == dump.status.cpu_wait
    assert int(a.GetProperty('cpu_throttled')) == dump.status.cpu_throttled

    net_bytes = a.GetProperty('net_bytes')
    for net_byte in dump.status.net_bytes.map:
        assert net_bytes.find('{}: '.format(net_byte.key, net_byte.val)) != -1
    assert net_bytes.count(';') + 1 == len(dump.status.net_bytes.map)

    net_drops = a.GetProperty('net_drops')
    for net_drop in dump.status.net_drops.map:
        assert net_drops.find('{}: '.format(net_drop.key, net_drop.val)) != -1
    assert (0 if len(net_drops) == 0 else (net_drops.count(';') + 1)) == len(dump.status.net_drops.map)

    assert dump.status.oom_kills == int(a.GetProperty('oom_kills'))

    assert dump.status.oom_kills_total == int(a.GetProperty('oom_kills_total'))

    assert dump.spec.oom_is_fatal == bool(a.GetProperty('oom_is_fatal'))

    assert dump.spec.oom_score_adj == int(a.GetProperty('oom_score_adj'))

    assert dump.status.parent == a.GetProperty('parent')

    assert dump.status.root_pid == int(a.GetProperty('root_pid'))

    io_read = a.GetProperty('io_read')
    for io_rd in dump.status.io_read.map:
        assert io_read.find('{}: {}'.format(io_rd.key, io_rd.val)) != -1
    assert io_read.count(';') + 1 == len(dump.status.io_read.map)


    io_write = a.GetProperty('io_write')
    for io_wt in dump.status.io_write.map:
        assert io_write.find('{}: {}'.format(io_wt.key, io_wt.val)) != -1
    assert io_write.count(';') + 1 == len(dump.status.io_write.map)

    io_ops = a.GetProperty('io_ops')
    for io_op in dump.status.io_ops.map:
        assert io_ops.find('{}: {}'.format(io_op.key, io_op.val)) != -1
    assert io_ops.count(';') + 1 == len(dump.status.io_ops.map)

    io_time_stat = a.GetProperty('io_time')
    for io_ts in dump.status.io_time.map:
        assert io_time_stat.find('{}: {}'.format(io_ts.key, io_ts.val)) != -1
    assert (0 if len(io_time_stat) == 0 else (io_time_stat.count(';') + 1)) == len(dump.status.io_time.map)

    assert dump.status.creation_time == int(a.GetProperty('creation_time[raw]'))

    assert dump.status.start_time == int(a.GetProperty('start_time[raw]'))

    assert dump.status.process_count == int(a.GetProperty('process_count'))

    assert dump.status.thread_count == int(a.GetProperty('thread_count'))

    assert dump.spec.thread_limit == int(a.GetProperty('thread_limit'))

    a.Stop()
    dump = a.Dump()
    assert dump.status.time == int(a.GetProperty('time'))

    AsRoot()
    a.SetProperty('net', 'L3 veth')
    a.SetProperty('sysctl', "net.ipv6.conf.all.forwarding: 1; net.ipv4.tcp_fastopen: 1")
    AsAlice()

    dump = a.Dump()
    CopyProps(a, b)

    assert a.GetProperty('sysctl') == b.GetProperty('sysctl')
    assert a.GetProperty('net') == b.GetProperty('net')

    sysctls = a.GetProperty('sysctl')
    for sysctl in dump.spec.sysctl.map:
        assert sysctls.find('{}: {}'.format(sysctl.key, sysctl.val)) != -1
    assert sysctls.count(';') + 1 == len(dump.spec.sysctl.map)

    taints = a.GetProperty('taint')
    for taint in dump.status.taint:
        assert taints.find(taint.msg) != -1
    assert taints.count('\n') == len(dump.status.taint)

    a.SetProperty('command', 'echo 1')
    a.Start()

    assert a.Wait() == a.name
    dump = a.Dump()
    assert dump.status.state == "dead"
    assert dump.status.exit_status == int(a.GetProperty('exit_status'))
    assert dump.status.exit_code == int(a.GetProperty('exit_code'))

    assert dump.status.oom_killed == bool(a.GetProperty('oom_killed'))

    assert dump.status.core_dumped == bool(a.GetProperty('core_dumped'))

    assert dump.status.death_time == int(a.GetProperty('death_time[raw]'))

    assert dump.status.change_time == int(a.GetProperty('change_time[raw]'))

    a.Stop()
    a.SetProperty("command", "wrq")

    try:
        a.Start()
    except:
        pass

    dump = a.Dump()
    assert a.GetProperty('start_error').find(dump.status.start_error.msg) != -1

    a.Stop()

    a.SetProperty("net_guarantee", "default: 0")
    CopyProps(a, b)
    dump = a.Dump()

    assert a.GetProperty('net_guarantee') == b.GetProperty('net_guarantee')

    net_guarantees = a.GetProperty('net_guarantee')
    for n in dump.spec.net_guarantee.map:
        assert net_guarantees.find('{}: {}'.format(n.key, n.val)) != -1

    a.Destroy()


    def check(ct, state):
        ExpectEq(ct['command'], 'sleep inf')
        ExpectEq(ct['weak'], True)
        limit = int(ct['memory_guarantee'])
        if limit > 0:
            ExpectEq(limit, 1048576)
        else:
            ExpectEq(limit, 0)
        ExpectEq(a['state'], state)

    # Create from spec

    spec = porto.rpc_pb2.TContainerSpec()
    spec.name = container_name_a
    spec.command = 'sleep inf'
    spec.weak = True

    new_spec = porto.rpc_pb2.TContainerSpec()
    new_spec.name = container_name_a
    new_spec.memory_guarantee = 1048576

    for start, state in ((False, 'stopped'), (True, 'running')):
        a = c.CreateSpec(container=spec, start=start)
        check(a, state)

        a.Destroy()

    # Update from spec

    for start1 in (True, False):
        for start2 in (True, False):
            print("Case:", start1, start2)

            a = c.CreateSpec(container=spec, start=start1)
            check(a, 'running' if start1 else 'stopped')

            a = c.UpdateSpec(container=new_spec, start=start2)
            check(a, 'running' if start1 or start2 else 'stopped')
            a.Destroy()

finally:
    if a is not None:
        Catch(a.Destroy)
    if b is not None:
        Catch(b.Destroy)
    if ab is not None:
        Catch(ab.Destroy)

    AsRoot()
    ConfigurePortod('test-spec', '')
