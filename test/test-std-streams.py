import porto
from test_common import *

import contextlib
import ctypes
import errno
import fcntl
import json
import os
import select
import stat
import subprocess
import sys
import tempfile
import tty


def WatchOpens(path, cleanup):
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
    Expect(fd >= 0, os.strerror(ctypes.get_errno()))
    cleanup.callback(os.close, fd)
    Expect(libc.inotify_add_watch(fd, os.fsencode(path), 0x20) >= 0,  # IN_OPEN
           os.strerror(ctypes.get_errno()))
    return fd


def ExpectPtyHangup(master):
    ExpectEq(select.select([master], [], [], 10)[0], [master])
    try:
        ExpectEq(os.read(master, 1), b'')
    except OSError as error:
        ExpectEq(error.errno, errno.EIO)


def test_stream_after_reload(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn))
    fd = cleanup.enter_context(open(os.path.join(vol.path, 'stdout'), 'w+'))
    ct = cleanup.enter_context(CreateContainer(conn, 'foobar'))
    value = f'/dev/fd/{fd.fileno()}'
    ct['stdout_path'] = value
    ReloadPortod()
    ExpectEq(ct['stdout_path'], value)
    ct['command'] = 'echo "hello world"'
    ct.Start()
    ct.Wait(10)
    fd.seek(0)
    ExpectEq(fd.read().strip(), 'hello world')
    ReloadPortod()
    ExpectEq(ct['stdout_path'], value)


def test_empty_streams_after_reload(conn, cleanup):
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-empty-streams', command='sleep 60', weak=False,
        stdin_path='', stdout_path='', stderr_path=''))
    ReloadPortod()
    ExpectEq(ct['state'], 'running')
    for prop in ('stdin_path', 'stdout_path', 'stderr_path'):
        ExpectEq(ct[prop], '')

    ct.Stop()
    ct['command'] = "sh -c 'cat; echo output; echo error >&2'"
    ct.Start()
    ct.Wait(10)
    ExpectEq(ct['exit_code'], '0')


def test_missing_stream_data(conn, cleanup):
    for stream in ('stdout', 'stderr'):
        ExpectException(conn.GetProperty, porto.exceptions.InvalidData, '/', stream)

    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    for name, properties in (
            ('default', {}),
            ('custom', dict(cwd=directory, stdout_path='output', stderr_path='errors'))):
        ct = cleanup.enter_context(RunContainer(
            conn, 'test-missing-stream-data-' + name, command='true', wait=10, **properties))
        for stream in ('stdout', 'stderr'):
            ExpectEq(ct[stream], '')
            os.unlink(os.path.join(ct['cwd'], ct[stream + '_path']))
            ExpectException(ct.GetProperty, porto.exceptions.InvalidData, stream)
        os.rmdir(ct['cwd'])
        for stream in ('stdout', 'stderr'):
            ExpectException(ct.GetProperty, porto.exceptions.InvalidData, stream)


def test_small_output(conn, cleanup):
    blksize = os.stat(__file__).st_blksize
    for limit in (blksize // 10, blksize, blksize * 10):
        ct = cleanup.enter_context(RunContainer(
            conn, 'test-small-output-{}'.format(limit), command='echo Hello', stdout_limit=limit, wait=10))
        ExpectEq(ct['stdout'], 'Hello\n')
        ExpectEq(ct['stdout[:0]'], '')
        ExpectEq(ct['stdout[1000:10]'], '')


def test_output_limit(conn, cleanup):
    blksize = os.stat(__file__).st_blksize
    output = 'a' * (blksize // 4 * 3)
    limit = blksize // 2
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-output-limit', command='echo ' + output, stdout_limit=limit, wait=10))
    # Reading is limited even when the file is too small for on-disk rotation.
    ExpectEq(ct['stdout'], output[-limit + 1:] + '\n')
    with open(os.path.join(ct['cwd'], ct['stdout_path'])) as source:
        ExpectEq(source.read(), output + '\n')


def test_read_limit(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn, private='test-std-stream'))
    part1 = 'a' * (16 << 20)
    part2 = 'b' * (16 << 20)
    with open(os.path.join(vol.path, 'test'), 'w') as source:
        source.write(part1 + part2)
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-read-limit', cwd=vol.path, stdout_limit=32 << 20, command='cat test', wait=30))
    ExpectEq(ct['stdout'], part2)
    ExpectEq(ct['stdout[0]'], part1)
    ExpectEq(ct['stdout[{}]'.format(8 << 20)], part1[8 << 20:] + part2[:8 << 20])
    ExpectEq(ct['stdout[:{}]'.format(8 << 20)], part2[:8 << 20])
    ExpectEq(ct['stdout[:{}]'.format(50 << 20)], part2)
    ExpectEq(ct['stdout[{}:{}]'.format(12 << 20, 8 << 20)], part1[12 << 20:] + part2[:4 << 20])
    for bounds in ('invalid', '0:invalid', '-1', ':-1'):
        ExpectException(ct.GetProperty, porto.exceptions.InvalidValue, 'stdout[{}]'.format(bounds))


def test_fifo_stdin(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    path = os.path.join(directory, 'input')
    os.mkfifo(path)
    # Opening stdin must not wait for a writer, but the installed FD must block.
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-fifo-stdin', stdin_path=path,
        command="python3 -c 'import os; assert os.get_blocking(0)'", wait=10))
    ExpectEq(ct['exit_code'], '0')


def test_fifo_streams(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    path = os.path.join(directory, 'fifo')
    os.mkfifo(path)
    fd = os.open(path, os.O_RDWR)
    cleanup.callback(os.close, fd)
    for source in (path, '/dev/fd/{}'.format(fd)):
        with RunContainer(
                conn, 'test-fifo-streams', stdin_path=source, stdout_path=source, stderr_path=source,
                command="python3 -c 'import os; "
                        "assert all(os.get_blocking(fd) for fd in (0, 1, 2)); "
                        "os.write(1, b\"out\"); os.write(2, b\"err\")'", wait=10) as ct:
            ExpectEq(ct['exit_code'], '0')
            ExpectException(ct.GetProperty, porto.exceptions.InvalidData, 'stdout')
            ExpectException(ct.GetProperty, porto.exceptions.InvalidData, 'stderr')
            ExpectEq(select.select([fd], [], [], 10)[0], [fd])
            ExpectEq(os.read(fd, 4096), b'outerr')


def test_fifo_stdout_without_reader(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn, backend='overlay', layers=['ubuntu-noble']))
    os.mkfifo(os.path.join(vol.path, 'stdout'))
    ExpectException(
        cleanup.enter_context, porto.exceptions.InvalidValue,
        RunContainer(conn, 'test', stdout_path='/stdout', root=vol.path, command='echo hello world'))


# Redirects use the starting client's descriptor, checked against the identity saved at Set.
def test_redirect_io(conn, cleanup):
    source = cleanup.enter_context(tempfile.TemporaryFile())
    output = cleanup.enter_context(tempfile.TemporaryFile())
    source.write(b'input\n')
    source.flush()
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-redirect', command="sh -c 'cat; printf error >&2'",
        stdin_path='/dev/fd/{}'.format(source.fileno()),
        stdout_path='/dev/fd/{}'.format(output.fileno()),
        stderr_path='/dev/fd/{}'.format(output.fileno()), wait=10))
    ExpectEq(ct['exit_code'], '0')
    output.seek(0)
    ExpectEq(output.read(), b'input\nerror')


def test_redirect_task_user(conn, cleanup):
    source = cleanup.enter_context(tempfile.TemporaryFile())
    output = cleanup.enter_context(tempfile.TemporaryFile())
    source.write(b'delegated\n')
    source.flush()
    master, slave = os.openpty()
    cleanup.callback(os.close, master)
    cleanup.callback(os.close, slave)
    tty.setraw(slave)

    # The root client can delegate its files and terminal to a nonroot task.
    for fd in (output.fileno(), slave):
        ct = cleanup.enter_context(RunContainer(
            conn, 'test-redirect-task-user-{}'.format(fd), user='porto-alice', group='porto-alice',
            command='cat', stdin_path='/dev/fd/{}'.format(source.fileno()),
            stdout_path='/dev/fd/{}'.format(fd), wait=10))
        ExpectEq(ct['exit_code'], '0')

    output.seek(0)
    ExpectEq(output.read(), b'delegated\n')
    ExpectEq(select.select([master], [], [], 10)[0], [master])
    ExpectEq(os.read(master, 4096), b'delegated\n')


def test_redirect_device_permissions(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory(dir='/dev'))
    os.mknod(os.path.join(directory, 'zero'), stat.S_IFCHR | 0o600, os.stat('/dev/zero').st_rdev)
    source = os.open(os.path.join(directory, 'zero'), os.O_PATH | os.O_CLOEXEC)
    cleanup.callback(os.close, source)
    watch = WatchOpens(os.path.join(directory, 'zero'), cleanup)
    command = "python3 -c 'import os; assert os.read(0, 1) == bytes(1)'"
    allowed = cleanup.enter_context(RunContainer(
        conn, 'test-redirect-device-allowed', isolate=False,
        stdin_path='/dev/fd/{}'.format(source), command=command, wait=10))
    ExpectEq(allowed['exit_code'], '0')
    ExpectEq(select.select([watch], [], [], 0)[0], [watch])
    os.read(watch, 4096)

    parent = cleanup.enter_context(RunContainer(
        conn, 'test-redirect-device-denied', isolate=False, devices='/dev/zero -'))
    child = cleanup.enter_context(CreateContainer(conn, parent.name + '/child'))
    child.Set(isolate=False, stdin_path='/dev/fd/{}'.format(source), command=command)
    try:
        child.Start()
    except porto.exceptions.Permission:
        pass
    else:
        raise AssertionError('redirect bypassed inherited device restriction')
    ExpectEq(select.select([watch], [], [], 0)[0], [])


def test_redirect_closed(conn, cleanup):
    output = cleanup.enter_context(tempfile.TemporaryFile())
    ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-closed'))
    # Keep the descriptor away from descriptors allocated by the RPC client.
    fd = fcntl.fcntl(output.fileno(), fcntl.F_DUPFD_CLOEXEC, 256)
    try:
        ct['command'] = 'echo unexpected'
        ct['stdout_path'] = '/dev/fd/{}'.format(fd)
    finally:
        os.close(fd)
    ExpectException(ct.Start, porto.exceptions.InvalidValue)
    ExpectEq(output.read(), b'')


def test_output_setgid_directory(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.chown(directory, alice_uid, bob_gid)
    os.chmod(directory, 0o2775)
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-output-setgid-directory', cwd=directory,
        user='porto-alice', group='porto-alice',
        stdout_path='stdout', stderr_path=os.path.join(directory, 'stderr'),
        command="sh -c 'echo stdout; echo stderr >&2'", wait=10))
    ExpectEq(ct['exit_code'], '0')
    for stream in ('stdout', 'stderr'):
        path = os.path.join(directory, stream)
        st = os.stat(path)
        ExpectEq((st.st_uid, st.st_gid), (alice_uid, alice_gid))
        ExpectEq(stat.S_IMODE(st.st_mode), 0o660)
        with open(path) as output:
            ExpectEq(output.read(), stream + '\n')


def test_existing_output_unchanged(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.chown(directory, alice_uid, bob_gid)
    os.chmod(directory, 0o2775)
    path = os.path.join(directory, 'output')
    with open(path, 'w') as output:
        output.write('existing\n')
    os.chown(path, bob_uid, bob_gid)
    os.chmod(path, 0o666)
    os.symlink('output', os.path.join(directory, 'errors'))
    before = os.stat(path)
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-existing-output-unchanged', cwd=directory,
        user='porto-alice', group='porto-alice',
        stdout_path='output', stderr_path='errors',
        command="sh -c 'echo stdout; echo stderr >&2'", wait=10))
    ExpectEq(ct['exit_code'], '0')
    after = os.stat(path)
    ExpectEq((after.st_ino, after.st_uid, after.st_gid, after.st_mode),
             (before.st_ino, before.st_uid, before.st_gid, before.st_mode))
    with open(path) as output:
        ExpectEq(output.read(), 'existing\nstdout\nstderr\n')


def test_dangling_output_symlink(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.symlink('missing', os.path.join(directory, 'output'))
    ct = cleanup.enter_context(CreateContainer(conn, 'test-dangling-output-symlink'))
    ct.Set(cwd=directory, stdout_path='output', command='true')
    ExpectException(ct.Start, porto.exceptions.InvalidValue)
    ExpectEq(os.path.lexists(os.path.join(directory, 'missing')), False)


def test_missing_input_not_created(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    path = os.path.join(directory, 'missing')
    ct = cleanup.enter_context(CreateContainer(conn, 'test-missing-input'))
    master, slave = os.openpty()
    cleanup.callback(os.close, master)
    try:
        ct['stdin_path'] = path
        ct['stdout_path'] = '/dev/fd/{}'.format(slave)
        ct['command'] = 'cat'
        # The child fails opening stdin after portod has acquired stdout.
        ExpectException(ct.Start, porto.exceptions.InvalidValue)
    finally:
        os.close(slave)
    ExpectEq(os.path.lexists(path), False)
    ExpectPtyHangup(master)


def test_redirect_permissions(conn, cleanup):
    output = cleanup.enter_context(tempfile.TemporaryFile())
    output.write(b'private\n')
    output.flush()
    ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-permissions'))
    ct.Set(owner_user='porto-alice', user='porto-alice', group='porto-alice', command='echo forbidden')
    watch = WatchOpens('/proc/self/fd/{}'.format(output.fileno()), cleanup)
    # Possession of an inherited descriptor does not grant permission to redirect it.
    subprocess.check_call([sys.executable, '-c', '''
import os, porto, sys
ct = porto.Connection().Find(sys.argv[1])
ct['stdout_path'] = os.path.join('/dev/fd', sys.argv[2])
try:
    ct.Start()
except porto.exceptions.Permission as error:
    assert 'Not enough permissions for redirect' in str(error), str(error)
else:
    raise AssertionError('unauthorized redirect accepted')
''', ct.name, str(output.fileno())], pass_fds=(output.fileno(),),
        user=alice_uid, group=alice_gid, extra_groups=[porto_gid])
    ExpectEq(select.select([watch], [], [], 0)[0], [])
    output.seek(0)
    ExpectEq(output.read(), b'private\n')


def test_redirect_client(conn, cleanup):
    original = cleanup.enter_context(tempfile.TemporaryFile())
    replacement = cleanup.enter_context(tempfile.TemporaryFile())
    ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-client'))
    ct['command'] = 'echo owner'
    ct['stdout_path'] = '/dev/fd/{}'.format(original.fileno())
    watch = WatchOpens('/proc/self/fd/{}'.format(replacement.fileno()), cleanup)
    subprocess.check_call([sys.executable, '-c',
        "import os, porto, sys; os.dup2(int(sys.argv[1]), int(sys.argv[2])); "
        "\ntry: porto.Connection().Start('test-redirect-client')"
        "\nexcept porto.exceptions.Permission: pass"
        "\nelse: raise AssertionError('different redirect accepted')",
        str(replacement.fileno()), str(original.fileno())],
        pass_fds=(replacement.fileno(), original.fileno()))
    ExpectEq(select.select([watch], [], [], 0)[0], [])
    ct.Start()
    ct.Wait(10)
    ExpectEq(original.read(), b'owner\n')
    ExpectEq(replacement.read(), b'')


def test_redirect_client_gone(conn, cleanup):
    output = cleanup.enter_context(tempfile.TemporaryFile())
    ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-gone'))
    ct['command'] = 'echo pinned'
    subprocess.check_call([sys.executable, '-c',
        "import os, porto, sys; porto.Connection().SetProperty("
        "'test-redirect-gone', 'stdout_path', os.path.join('/dev/fd', sys.argv[1]))",
        str(output.fileno())], pass_fds=(output.fileno(),))
    ct.Start()
    ct.Wait(10)
    ExpectEq(output.read(), b'pinned\n')


def test_redirect_replaced(conn, cleanup):
    original = cleanup.enter_context(tempfile.TemporaryFile())
    replacement = cleanup.enter_context(tempfile.TemporaryFile())
    saved = cleanup.enter_context(os.fdopen(os.dup(original.fileno()), 'rb'))
    ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-replaced'))
    ct['command'] = 'echo saved'
    ct['stdout_path'] = '/dev/fd/{}'.format(original.fileno())
    os.dup2(replacement.fileno(), original.fileno())
    ReloadPortod()
    ExpectException(ct.Start, porto.exceptions.Permission)
    ExpectEq(saved.read(), b'')
    ExpectEq(replacement.read(), b'')


def test_stream_locations(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn, backend='overlay', layers=['ubuntu-noble']))
    work = os.path.join(vol.path, 'work')
    os.mkdir(work)
    os.chown(work, alice_uid, alice_gid)
    with open(os.path.join(work, 'input'), 'w') as source:
        source.write('inside\n')
    errors = cleanup.enter_context(tempfile.TemporaryFile())
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-locations', root=vol.path, cwd='/work',
        user='porto-alice', group='porto-alice',
        command="sh -c 'cat; printf redirected >&2'", stdin_path='input',
        stdout_path='stdout', stderr_path='/dev/fd/{}'.format(errors.fileno()), wait=10))
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'inside\n')
    ExpectEq(errors.read(), b'redirected')
    st = os.stat(os.path.join(work, 'stdout'))
    ExpectEq((st.st_uid, st.st_gid), (alice_uid, alice_gid))
    ct.Stop()
    ct['stderr_path'] = '/dev/fd/{}'.format(errors.fileno())
    ct.Start()
    ct.Wait(10)
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'inside\ninside\n')


def test_default_stream_location(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn, backend='overlay', layers=['ubuntu-noble']))
    os.mkdir(os.path.join(vol.path, 'work'))
    os.chown(os.path.join(vol.path, 'work'), alice_uid, alice_gid)
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-default-stream-location', root=vol.path, cwd='/work',
        user='porto-alice', group='porto-alice', command='echo host-log', wait=10))
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'host-log\n')
    ExpectEq(os.path.exists(os.path.join(vol.path, 'work', 'stdout')), False)
    ct.Stop()
    ct.Start()
    ct.Wait(10)
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'host-log\n')


def test_nested_default_streams(conn, cleanup):
    parent = cleanup.enter_context(RunContainer(
        conn, 'test-nested-default-streams', command='sleep 60', net='none',
        user='porto-alice', group='porto-alice'))
    directory = parent['cwd']
    cleanup.callback(os.chmod, directory, os.stat(directory).st_mode & 0o7777)
    os.chmod(directory, 0o700)

    child = cleanup.enter_context(RunContainer(
        conn, parent.name + '/child', net='none',
        user='porto-bob', group='porto-bob',
        command="sh -c 'echo output; echo error >&2'", wait=10))
    ExpectEq(child['exit_code'], '0')
    ExpectEq(child['stdout'], 'output\n')
    ExpectEq(child['stderr'], 'error\n')
    for stream in ('stdout', 'stderr'):
        st = os.stat(os.path.join(child['cwd'], child[stream + '_path']))
        ExpectEq((st.st_uid, st.st_gid), (bob_uid, bob_gid))


def test_default_stream_symlink_access(conn, cleanup):
    private = cleanup.enter_context(tempfile.TemporaryDirectory())
    with open(os.path.join(private, 'stdout'), 'wb') as output:
        output.write(b'x' * 65536)
    os.chmod(os.path.join(private, 'stdout'), 0o600)
    parent = cleanup.enter_context(RunContainer(
        conn, 'test-default-stream-symlink', command='sleep 60', net='none',
        user='porto-alice', group='porto-alice'))
    child = cleanup.enter_context(RunContainer(
        conn, parent.name + '/child', command='sleep 60', net='none',
        user='porto-alice', group='porto-alice', stdout_limit=4096))
    ExpectEq(child['stdout'], '')
    directory = child['cwd']

    subprocess.check_call(['mv', directory, directory + '.original'],
                          user=alice_uid, group=alice_gid, extra_groups=[])
    cleanup.callback(os.rename, directory + '.original', directory)
    subprocess.check_call(['ln', '-s', private, directory],
                          user=alice_uid, group=alice_gid, extra_groups=[])
    cleanup.callback(os.unlink, directory)

    # Reaping rotates the logs before the container becomes dead.
    child.Kill(signal.SIGKILL)
    child.Wait(10)
    ExpectEq(child['state'], 'dead')
    ExpectException(child.GetProperty, porto.exceptions.Unknown, 'stdout')
    with open(os.path.join(private, 'stdout'), 'rb') as output:
        ExpectEq(output.read(), b'x' * 65536)


def test_default_stream_startup_symlink(conn, cleanup):
    private = cleanup.enter_context(tempfile.TemporaryDirectory())
    with open(os.path.join(private, 'stdout'), 'wb') as output:
        output.write(b'private\n')
        os.fchmod(output.fileno(), 0o600)
    parent = cleanup.enter_context(RunContainer(
        conn, 'test-default-stream-startup-symlink', command='sleep 60', net='none',
        user='porto-alice', group='porto-alice'))
    child = cleanup.enter_context(RunContainer(
        conn, parent.name + '/child', cwd='/', net='none',
        user='porto-alice', group='porto-alice', respawn=True, max_respawns=1,
        command="sh -c 'echo output; echo error >&2; sleep 60'"))
    directory = os.path.join(parent['cwd'], 'child')

    subprocess.check_call(['mv', directory, directory + '.original'],
                          user=alice_uid, group=alice_gid, extra_groups=[])
    cleanup.callback(os.rename, directory + '.original', directory)
    subprocess.check_call(['ln', '-s', private, directory],
                          user=alice_uid, group=alice_gid, extra_groups=[])
    cleanup.callback(os.unlink, directory)

    # Respawn reopens the default logs without recreating the workdir.
    child.Kill(signal.SIGKILL)
    deadline = time.monotonic() + 10
    while child['respawn_count'] != '1' or child['state'] not in ('running', 'dead'):
        Expect(time.monotonic() < deadline, 'respawn did not complete')
        time.sleep(0.05)

    # Neither appending to an existing file nor creating a new one is allowed.
    with open(os.path.join(private, 'stdout'), 'rb') as output:
        ExpectEq(output.read(), b'private\n')
    ExpectEq(os.path.exists(os.path.join(private, 'stderr')), False)
    ExpectEq(child['state'], 'dead')
    ExpectIn(os.strerror(errno.ELOOP), child['start_error'])


def test_default_stream_private_data(conn, cleanup):
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-default-stream-private-data', command='sleep 60', net='none',
        owner_user='porto-alice', user='porto-alice', group='porto-alice', stdout_limit=4096))
    for stream in ('stdout', 'stderr'):
        ExpectEq(ct[stream], '')
        with open(os.path.join(ct['cwd'], ct[stream + '_path']), 'wb') as file:
            # Alice may write this host log, but only root may read its contents.
            os.fchown(file.fileno(), 0, alice_gid)
            os.fchmod(file.fileno(), 0o620)
            file.write(b'private\n' * 8192)

    ct.Kill(signal.SIGKILL)
    ct.WaitContainer(timeout=10)
    for stream in ('stdout', 'stderr'):
        Expect(int(ct[stream + '_offset']) > 0)

    # Rotation's privileged reopen must not grant read access through the API.
    subprocess.check_call([sys.executable, '-c', '''
import porto, sys
ct = porto.Connection().Find(sys.argv[1])
assert ct['state'] == 'dead'
for stream in ('stdout', 'stderr'):
    for prop in (stream, '{}[{}:8]'.format(stream, ct[stream + '_offset'])):
        try:
            ct[prop]
        except porto.exceptions.Unknown as error:
            assert 'Permission denied:' in str(error), str(error)
        else:
            raise AssertionError(prop + ' exposed private host log data')
''', ct.name], user=alice_uid, group=alice_gid, extra_groups=[porto_gid])


def test_stream_permissions(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn, backend='overlay', layers=['ubuntu-noble']))
    blocked = os.path.join(vol.path, 'blocked')
    os.mkdir(blocked, 0o700)
    ExpectException(
        cleanup.enter_context, porto.exceptions.InvalidValue,
        RunContainer(conn, 'test-stream-permissions', root=vol.path, cwd='/',
                     user='porto-alice', group='porto-alice', command='echo denied', stdout_path='/blocked/output'))
    ExpectEq(os.path.exists(os.path.join(blocked, 'output')), False)


def test_terminal_bidirectional_io(conn, cleanup):
    master, slave = os.openpty()
    cleanup.callback(os.close, master)
    cleanup.callback(os.close, slave)
    tty.setraw(slave)
    for path in ('/dev/fd/{}'.format(slave), os.ttyname(slave)):
        for target in ('stdin_path', 'stderr_path'):
            if target == 'stderr_path':
                os.write(master, b'input\n')
            with contextlib.ExitStack() as case:
                ct = case.enter_context(RunContainer(
                    conn, 'test-terminal-bidirectional', isolate=False,
                    command={
                        'stdin_path': "sh -c 'printf output >&0'",
                        'stderr_path': "sh -c 'read -r value <&2 && test \"$value\" = input'",
                    }[target], **{target: path}, wait=10))
                ExpectEq(ct['exit_code'], '0')
                if target == 'stdin_path':
                    ExpectEq(select.select([master], [], [], 10)[0], [master])
                    ExpectEq(os.read(master, 6), b'output')


def test_distinct_terminals(conn, cleanup):
    masters, slaves = [], []
    for _ in range(3):
        master, slave = os.openpty()
        cleanup.callback(os.close, master)
        cleanup.callback(os.close, slave)
        tty.setraw(slave)
        masters.append(master)
        slaves.append(slave)

    for paths in ([os.ttyname(slave) for slave in slaves],
                  ['/dev/fd/{}'.format(slave) for slave in slaves]):
        os.write(masters[0], b'input\n')
        with RunContainer(
                conn, 'test-distinct-terminals', isolate=False,
                stdin_path=paths[0], stdout_path=paths[1], stderr_path=paths[2],
                command="sh -ec 'read -r value; printf \"%s\" \"$value\"; printf error >&2'", wait=10) as ct:
            ExpectEq(ct['exit_code'], '0')
            ExpectEq(select.select([masters[1]], [], [], 10)[0], [masters[1]])
            ExpectEq(os.read(masters[1], 4096), b'input')
            ExpectEq(select.select([masters[2]], [], [], 10)[0], [masters[2]])
            ExpectEq(os.read(masters[2], 4096), b'error')


def test_terminal_owned_by_another_session(conn, cleanup):
    master, slave = os.openpty()
    cleanup.callback(os.close, master)
    cleanup.callback(os.close, slave)
    owner = cleanup.enter_context(subprocess.Popen(
        [sys.executable, '-c',
         'import fcntl, sys, termios; '
         'fcntl.ioctl(int(sys.argv[1]), termios.TIOCSCTTY, 0); '
         'print("ready", flush=True); sys.stdin.read()', str(slave)],
        pass_fds=(slave,), start_new_session=True,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True))
    cleanup.callback(owner.stdin.close)
    ExpectEq(select.select([owner.stdout], [], [], 10)[0], [owner.stdout])
    ExpectEq(owner.stdout.readline(), 'ready\n')

    for path in (os.ttyname(slave), '/dev/fd/{}'.format(slave)):
        with RunContainer(
                conn, 'test-owned-terminal', isolate=False, stdin_path=path,
                command="sh -c 'printf output >&0'", wait=10) as ct:
            ExpectEq(ct['exit_code'], '0')
            ExpectEq(select.select([master], [], [], 10)[0], [master])
            ExpectEq(os.read(master, 6), b'output')
            ExpectEq(owner.poll(), None)


# A dead or stopped container must not keep the PTY slave alive in portod.
def test_redirect_hangup(conn, cleanup):
    for command in ('true', 'sleep 60'):
        master, slave = os.openpty()
        cleanup.callback(os.close, master)
        redirect = '/dev/fd/{}'.format(slave)
        try:
            ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-hangup-' + command.split()[0]))
            ct['command'] = command
            for prop in ('stdin_path', 'stdout_path', 'stderr_path'):
                ct[prop] = redirect
            ct.Start()
        finally:
            os.close(slave)
        if command == 'true':
            ct.Wait(10)
            ExpectEq(ct['exit_code'], '0')
        else:
            ct.Stop()
        for prop in ('stdin_path', 'stdout_path', 'stderr_path'):
            ExpectEq(ct[prop], redirect)
        ExpectPtyHangup(master)


def test_redirect_start_failure(conn, cleanup):
    original = cleanup.enter_context(tempfile.TemporaryFile())
    replacement = cleanup.enter_context(tempfile.TemporaryFile())
    ct = cleanup.enter_context(CreateContainer(conn, 'test-redirect-failure'))
    master, slave = os.openpty()
    cleanup.callback(os.close, master)
    try:
        ct['command'] = 'true'
        ct['stdout_path'] = '/dev/fd/{}'.format(slave)
        ct['stderr_path'] = '/dev/fd/{}'.format(original.fileno())
        os.dup2(replacement.fileno(), original.fileno())
        ExpectException(ct.Start, porto.exceptions.Permission)
    finally:
        os.close(slave)
    # Failure opening stderr must release the already opened stdout.
    ExpectPtyHangup(master)


def test_stream_reconfiguration(conn, cleanup):
    directory = os.path.realpath(cleanup.enter_context(tempfile.TemporaryDirectory()))
    output = cleanup.enter_context(tempfile.TemporaryFile())
    ct = cleanup.enter_context(CreateContainer(conn, 'test-stream-reconfiguration'))
    ct['stdout_path'] = 'output'
    ct['stdout_path'] = '/dev/fd/{}'.format(output.fileno())
    ct['command'] = 'echo redirect'
    ct.Start()
    ct.Wait(10)
    ExpectEq(output.read(), b'redirect\n')
    ExpectException(ct.GetProperty, porto.exceptions.InvalidData, 'stdout')
    ct.Stop()

    ct['stdout_path'] = 'output'
    ExpectException(ct.SetProperty, porto.exceptions.InvalidValue, 'stdout_path', '/dev/fd/invalid')
    ExpectEq(ct['stdout_path'], 'output')
    # Resolve relative paths against the final cwd, not the cwd at Set.
    ct['cwd'] = directory
    ct['command'] = 'echo file'
    ct.Start()
    ct.Wait(10)
    ExpectEq(ct['stdout'], 'file\n')
    ReloadPortod()
    ExpectEq(ct['stdout'], 'file\n')
    ExpectEq(ct['stdout_path'], 'output')
    ct.Stop()

    ct['stdout_path'] = '/dev/null'
    ct.Start()
    ct.Wait(10)
    ExpectException(ct.GetProperty, porto.exceptions.InvalidData, 'stdout')


def test_stream_magic_links(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    with open(os.path.join(directory, 'input'), 'w') as source:
        source.write('ordinary symlink\n')
    os.symlink(os.path.join(directory, 'input'), os.path.join(directory, 'link'))
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-symlink', stdin_path=os.path.join(directory, 'link'), command='cat', wait=10))
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'ordinary symlink\n')

    os.symlink('/proc/self/fd/1', os.path.join(directory, 'magic'))
    for index, path in enumerate(('/proc/self/fd/1', os.path.join(directory, 'magic'))):
        for prop in ('stdin_path', 'stdout_path', 'stderr_path'):
            ct = cleanup.enter_context(CreateContainer(conn, 'test-stream-magic-link-{}-{}'.format(index, prop)))
            ct['command'] = 'sh -c "cat | head"'
            ct[prop] = path
            ExpectException(ct.Start, porto.exceptions.InvalidValue)


def test_stream_cwd_magic_links(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.symlink(directory, os.path.join(directory, 'ordinary'))
    os.symlink('/proc/self/root', os.path.join(directory, 'magic'))
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-cwd', cwd=os.path.join(directory, 'ordinary'), command='pwd', wait=10))
    ExpectEq(ct['stdout'].strip(), os.path.realpath(directory))
    for index, path in enumerate(('/proc/self/root', os.path.join(directory, 'magic'))):
        ct = cleanup.enter_context(CreateContainer(conn, 'test-stream-cwd-magic-' + str(index)))
        ct['command'] = 'true'
        ct['cwd'] = path
        ExpectException(ct.Start, porto.exceptions.InvalidPath)


def test_stream_host_null(conn, cleanup):
    original = os.stat('/dev/null')
    cleanup.callback(os.chown, '/dev/null', original.st_uid, original.st_gid)
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    marker = os.path.join(directory, 'null-marker')
    with open(marker, 'w') as output:
        output.write('container null\n')
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-host-null-input', bind=marker + ' /dev/null rw',
        user='porto-alice', group='porto-alice',
        stdin_path='/dev/null', command="sh -c 'cat; cat /dev/null'", wait=10))
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'container null\n')
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-host-null-output', bind=marker + ' /dev/null rw',
        user='porto-alice', group='porto-alice',
        stdout_path='/dev/null', stderr_path='/dev/null',
        command="sh -c 'echo output; echo error >&2'", wait=10))
    ExpectEq(ct['exit_code'], '0')
    with open(marker) as source:
        ExpectEq(source.read(), 'container null\n')
    ExpectException(ct.GetProperty, porto.exceptions.InvalidData, 'stdout')
    current = os.stat('/dev/null')
    ExpectEq((current.st_uid, current.st_gid), (original.st_uid, original.st_gid))


def test_stream_mode_defaults(conn, cleanup):
    ct = cleanup.enter_context(CreateContainer(conn, 'test-stream-mode-defaults'))
    for mode, stdout, stderr in (('app', 'stdout', 'stderr'),
                                ('os', '/dev/null', '/dev/null'),
                                ('app', 'stdout', 'stderr')):
        ct['virt_mode'] = mode
        ExpectEq(ct['stdout_path'], stdout)
        ExpectEq(ct['stderr_path'], stderr)
        ReloadPortod()
        ExpectEq(ct['stdout_path'], stdout)
        ExpectEq(ct['stderr_path'], stderr)

    ct['command'] = "sh -c 'printf output; printf error >&2'"
    ct.Start()
    ct.Wait(10)
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['stdout'], 'output')
    ExpectEq(ct['stderr'], 'error')
    ct.Stop()

    ct['stdout_path'] = 'explicit-out'
    ct['stderr_path'] = 'explicit-err'
    ct['virt_mode'] = 'os'
    ReloadPortod()
    ExpectEq(ct['stdout_path'], 'explicit-out')
    ExpectEq(ct['stderr_path'], 'explicit-err')


def test_stream_symlink_access(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    marker = os.path.join(directory, 'private', 'output')
    os.mkdir(os.path.dirname(marker), 0o700)
    with open(marker, 'wb') as source:
        source.write(b'x' * 65536)
    os.chmod(marker, 0o600)
    os.mkdir(os.path.join(directory, 'logs'))
    os.chown(os.path.join(directory, 'logs'), alice_uid, alice_gid)
    os.chown(directory, alice_uid, alice_gid)

    # Swap the log directory after stdout opens. Neither rotation nor reads
    # may access the private file through the replacement symlink.
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-symlink-access', cwd=directory,
        owner_user='porto-alice', user='porto-alice', group='porto-alice',
        stdout_path=os.path.join(directory, 'logs', 'output'), stdout_limit=4096,
        command="sh -ec 'mv logs original; ln -s private logs; ! cat logs/output'", wait=10))
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(os.path.getsize(marker), 65536)
    with open(marker, 'rb') as source:
        ExpectEq(source.read(), b'x' * 65536)

    subprocess.check_call([sys.executable, '-c', '''
import porto, sys
ct = porto.Connection().Find(sys.argv[1])
assert ct['exit_code'] == '0'
try:
    ct['stdout']
except porto.exceptions.Unknown as error:
    assert 'Permission denied:' in str(error), str(error)
else:
    raise AssertionError('stdout disclosed a private file')
''', ct.name], user=alice_uid, group=alice_gid, extra_groups=[porto_gid])


def test_stream_root_symlink_access(conn, cleanup):
    private = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.makedirs(os.path.join(private, 'public', 'root'), mode=0o755)
    for path in ('stdout', 'stderr', 'root/stdout', 'root/stderr'):
        with open(os.path.join(private, 'public', path), 'w') as source:
            source.write('private\n')
            os.fchmod(source.fileno(), 0o644)
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.chown(directory, alice_uid, alice_gid)

    for path in ('root', 'parent/root'):
        os.makedirs(os.path.join(directory, path), mode=0o755)
        for stream in ('stdout', 'stderr'):
            with open(os.path.join(directory, path, stream), 'w') as source:
                source.write('public\n')
                os.fchown(source.fileno(), alice_uid, alice_gid)
                os.fchmod(source.fileno(), 0o644)
        ct = cleanup.enter_context(RunContainer(
            conn, 'test-stream-root-' + path.replace('/', '-'),
            root=os.path.realpath(os.path.join(directory, path)),
            owner_user='porto-alice', user='porto-alice', group='porto-alice',
            stdout_path='/stdout', stderr_path='/stderr'))
        for stream in ('stdout', 'stderr'):
            ExpectEq(ct[stream], 'public\n')

        subprocess.check_call([sys.executable, '-c', '''
import errno, os, porto, sys
root, target, private, name = sys.argv[1:]
os.rename(target, target + '.original')
try:
    os.symlink(private, target)
    try:
        os.open(os.path.join(root, 'stdout'), os.O_RDONLY)
    except PermissionError:
        pass
    else:
        raise AssertionError('private root unexpectedly accessible')
    ct = porto.Connection().Find(name)
    for stream in ('stdout', 'stderr'):
        try:
            ct[stream]
        except porto.exceptions.Unknown as error:
            assert os.strerror(errno.ELOOP) in str(error), str(error)
        else:
            raise AssertionError(stream + ' disclosed a private file')
finally:
    os.unlink(target)
    os.rename(target + '.original', target)
''', os.path.join(directory, path), os.path.join(directory, path.split('/')[0]),
            os.path.join(private, 'public'), ct.name], user=alice_uid, group=alice_gid, extra_groups=[porto_gid])


def test_stream_root_private_parent(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.makedirs(os.path.join(directory, 'root', 'logs'), mode=0o755)
    os.symlink('logs', os.path.join(directory, 'root', 'link'))
    for stream in ('stdout', 'stderr'):
        with open(os.path.join(directory, 'root/logs', stream), 'w') as source:
            source.write(stream + '\n')
            os.fchown(source.fileno(), alice_uid, alice_gid)
            os.fchmod(source.fileno(), 0o644)

    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-root-private-parent', root=os.path.realpath(os.path.join(directory, 'root')),
        owner_user='porto-alice', user='porto-alice', group='porto-alice',
        stdout_path='/link/stdout', stderr_path='/link/stderr'))
    for stream in ('stdout', 'stderr'):
        ExpectEq(ct[stream], stream + '\n')


def test_stream_device_access(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory(dir='/dev'))
    os.chown(directory, alice_uid, alice_gid)
    os.mkdir(os.path.join(directory, 'logs'))
    os.chown(os.path.join(directory, 'logs'), alice_uid, alice_gid)
    os.mkdir(os.path.join(directory, 'devices'))
    for stream in ('stdout', 'stderr'):
        os.mknod(os.path.join(directory, 'devices', stream), stat.S_IFCHR | 0o666, os.stat('/dev/zero').st_rdev)
        os.chmod(os.path.join(directory, 'devices', stream), 0o666)
    watch = WatchOpens(os.path.join(directory, 'devices'), cleanup)

    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-device-access', isolate=False, net='none', cwd=directory,
        owner_user='porto-alice', user='porto-alice', group='porto-alice', devices='/dev/zero -',
        stdout_path='logs/stdout', stderr_path='logs/stderr', wait=10, command="""python3 -c '
import errno, os
try:
    os.open("devices/stdout", os.O_RDONLY)
except OSError as error:
    assert error.errno == errno.EPERM, error
else:
    raise AssertionError("device unexpectedly allowed")
os.rename("logs", "original")
os.symlink("devices", "logs")
'"""))
    ExpectEq(ct['exit_code'], '0')
    ExpectEq(ct['state'], 'dead')

    subprocess.check_call([sys.executable, '-c', '''
import porto, sys
ct = porto.Connection().Find(sys.argv[1])
for stream in ('stdout', 'stderr'):
    try:
        ct[stream]
    except porto.exceptions.InvalidData:
        pass
    else:
        raise AssertionError(stream + ' accepted a device')
''', ct.name], user=alice_uid, group=alice_gid, extra_groups=[porto_gid])
    # Neither reaping rotation nor API reads may open the rejected devices.
    ExpectEq(select.select([watch], [], [], 0)[0], [])


def test_stream_rotation_write_only(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    os.chown(directory, bob_uid, bob_gid)
    block = os.stat(directory).st_blksize
    with open(os.path.join(directory, 'input'), 'w') as file:
        file.write('x' * (16 * block))
        os.fchown(file.fileno(), bob_uid, bob_gid)
    for stream in ('stdout', 'stderr'):
        path = os.path.join(directory, stream)
        with open(path, 'w') as file:
            os.fchown(file.fileno(), bob_uid, bob_gid)
            os.fchmod(file.fileno(), 0o200)

    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-rotation-write-only', cwd=directory,
        user='porto-bob', group='porto-bob', stdout_path='stdout', stderr_path='stderr',
        command="sh -ec 'cat input; cat input >&2'", stdout_limit=2 * block, wait=10))
    ExpectEq(ct['exit_code'], '0')
    for stream in ('stdout', 'stderr'):
        Expect(int(ct[stream + '_offset']) > 0)
        Expect(os.path.getsize(os.path.join(directory, stream)) <= 2 * block)
        ExpectEq(os.stat(os.path.join(directory, stream)).st_mode & 0o777, 0o200)


def test_stream_rotation_records(conn, cleanup):
    directory = cleanup.enter_context(tempfile.TemporaryDirectory())
    block = os.stat(directory).st_blksize
    # Records exceed one block, so collapsing whole blocks cuts through a record.
    record = json.dumps({'message': 'x' * block}) + '\n'
    with open(os.path.join(directory, 'input'), 'w') as file:
        file.write(record * 16)
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-rotation-records', cwd=directory,
        stdout_path='stdout', stderr_path='stderr',
        command="sh -ec 'cat input; cat input >&2'", stdout_limit=2 * block, wait=10))
    ExpectEq(ct['exit_code'], '0')
    for stream in ('stdout', 'stderr'):
        Expect(int(ct[stream + '_offset']) > 0)
        with open(os.path.join(directory, stream)) as file:
            text = file.read()
        # Filesystems without range collapse use the existing truncate fallback.
        if text.strip():
            ExpectEq(json.loads(text), json.loads(record))


def test_stream_rotation_restart(conn, cleanup):
    ct = cleanup.enter_context(RunContainer(
        conn, 'test-stream-rotation', command='echo ' + 'x' * 65536, stdout_limit=4096, wait=10))
    offset = int(ct['stdout_offset'])
    Expect(offset > 0)
    ExpectException(ct.GetProperty, porto.exceptions.InvalidData, 'stdout[0]')
    ExpectEq(ct['stdout[{}:0]'.format(offset)], '')
    with open(os.path.join(ct['cwd'], ct['stdout_path'])) as output:
        ExpectEq(ct['stdout[{}:3]'.format(offset)], output.read(3))
    ct.Stop()
    ct['command'] = 'echo restarted'
    ct.Start()
    ct.Wait(10)
    ExpectEq(ct['stdout_offset'], '0')
    ExpectEq(ct['stdout'], 'restarted\n')


if __name__ == '__main__':
    conn = porto.Connection(timeout=30)
    for test in (
        test_missing_stream_data,
        test_small_output,
        test_output_limit,
        test_read_limit,
        test_fifo_stdin,
        test_fifo_streams,
        test_fifo_stdout_without_reader,
        test_redirect_io,
        test_redirect_task_user,
        test_redirect_device_permissions,
        test_redirect_closed,
        test_output_setgid_directory,
        test_existing_output_unchanged,
        test_dangling_output_symlink,
        test_missing_input_not_created,
        test_redirect_permissions,
        test_redirect_replaced,
        test_redirect_client,
        test_redirect_client_gone,
        test_stream_locations,
        test_default_stream_location,
        test_nested_default_streams,
        test_default_stream_symlink_access,
        test_default_stream_startup_symlink,
        test_default_stream_private_data,
        test_stream_permissions,
        test_terminal_bidirectional_io,
        test_distinct_terminals,
        test_terminal_owned_by_another_session,
        test_redirect_hangup,
        test_redirect_start_failure,
        test_stream_reconfiguration,
        test_stream_magic_links,
        test_stream_cwd_magic_links,
        test_stream_host_null,
        test_stream_mode_defaults,
        test_stream_after_reload,
        test_empty_streams_after_reload,
        test_stream_symlink_access,
        test_stream_root_symlink_access,
        test_stream_root_private_parent,
        test_stream_device_access,
        test_stream_rotation_write_only,
        test_stream_rotation_records,
        test_stream_rotation_restart,
    ):
        with contextlib.ExitStack() as cleanup:
            test(conn, cleanup)
