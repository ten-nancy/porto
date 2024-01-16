import subprocess
import tempfile
import contextlib
import random
import porto
import os
from string import ascii_lowercase
from itertools import chain
import signal
import time

from test_common import ExpectEq, ExpectException, ExpectLe


def subpids(pid):
    output = subprocess.check_output(['pgrep', '-P', str(pid)])
    return list(map(int, output.splitlines()))


def populate(root, tree):
    if isinstance(tree, str):
        with open(root, 'w') as f:
            return f.write(tree)
    try:
        os.mkdir(root)
    except FileExistsError:
        pass
    for k, v in tree.items():
        populate(os.path.join(root, k), v)


@contextlib.contextmanager
def ext4_image(tree):
    with tempfile.NamedTemporaryFile() as image:
        image.truncate(1<<20)
        subprocess.check_output(['mkfs.ext4', image.name])
        with tempfile.TemporaryDirectory() as imagedir:
            subprocess.check_call(['mount', '-o', 'loop', image.name, imagedir])
            try:
                populate(imagedir, tree)
            finally:
                subprocess.check_output(['umount', imagedir])
        yield image.name


@contextlib.contextmanager
def invalid_image():
    with tempfile.NamedTemporaryFile() as image:
        image.write(b'1'*1024)
        yield image.name


@contextlib.contextmanager
def process(pid):
    try:
        yield pid
    finally:
        os.kill(pid, signal.SIGTERM)
        start = time.time()
        while True:
            if time.time() - start > 5:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            try:
                os.lstat('/proc/{}'.format(pid))
                time.sleep(0.1)
            except FileNotFoundError:
                break



@contextlib.contextmanager
def path(p):
    try:
        yield p
    finally:
        try:
            os.remove(p)
        except FileNotFoundError:
            pass


@contextlib.contextmanager
def nbd_server(images):
    suffix = ''.join(random.choice(ascii_lowercase) for _ in range(12))
    socket_path = os.path.join(tempfile.gettempdir(), 'nbd-server-{}.socket'.format(suffix))
    pid_path = os.path.join(tempfile.gettempdir(), 'nbd-server-{}.pid'.format(suffix))
    with tempfile.NamedTemporaryFile(mode='w') as fp:
        images_config = '\n'.join(
            '''
            [{name}]
                exportname = {image_path}
            '''.format(name=name, image_path=image_path)
            for name, image_path in images.items()
        )
        fp.write(
            '''
            [generic]
                unixsock = {unixsock}
            {images}
            '''.format(unixsock=socket_path, images=images_config))
        fp.flush()
        subprocess.check_output(['nbd-server', '-C', fp.name, '-p', pid_path])
        with open(pid_path) as f:
            pid = int(f.read().strip())
        os.lstat('/proc/{}'.format(pid))
        with process(pid), path(socket_path), path(pid_path):
            yield pid, socket_path


@contextlib.contextmanager
def freezer(pids):
    stopped = []
    try:
        for pid in pids:
            os.kill(pid, signal.SIGSTOP)
            stopped.append(pid)
        yield
    finally:
        for pid in stopped:
            os.kill(pid, signal.SIGCONT)


def readall(path):
    with open(path) as f:
        return f.read()


class TestNbdVolume:
    def __init__(self, cleanup):
        self.cleanup = cleanup

    def setup(self):
        self.image_path = self.cleanup.enter_context(ext4_image({'foo': 'bar'}))
        self.invalid_impage_path = self.cleanup.enter_context(invalid_image())
        self.nbd_pid, self.socket_path = self.cleanup.enter_context(nbd_server({
            'image': self.image_path,
            'invalid': self.invalid_impage_path,
        }))
        self.conn = porto.Connection(timeout=10)

    def run(self):
        # order matters
        ExpectException(self.test_invalid_socket, porto.exceptions.NbdSocketUnavaliable);
        ExpectException(self.test_non_existent_export, porto.exceptions.NbdUnkownExport);
        ExpectException(self.test_invalid_image, porto.exceptions.InvalidFilesystem)
        ExpectException(self.test_connection_timeout, porto.exceptions.NbdSocketTimeout)
        vol = self.test_volume_creation()
        self.test_io_timeout(vol)
        self.test_reconnect_read(vol)

    @contextlib.contextmanager
    def createVolume(self, *args, **kwargs):
        vol = self.conn.CreateVolume(*args, **kwargs)
        try:
            yield vol
        finally:
            self.conn.UnlinkVolume(vol.path)

    def test_invalid_socket(self):
        self.cleanup.enter_context(self.createVolume(
            backend='nbd',
            storage='unix+tcp:/lol/kek/cheburek?export=image'))

    def test_non_existent_export(self):
        self.cleanup.enter_context(self.createVolume(
            backend='nbd',
            storage='unix+tcp:{path}?export=lol-kek-cheburek'.format(path=self.socket_path)))

    def test_invalid_image(self):
        self.cleanup.enter_context(self.createVolume(
            backend='nbd',
            storage='unix+tcp:{path}?export=invalid'.format(path=self.socket_path)))

    def test_connection_timeout(self):
        with freezer([self.nbd_pid]):
            self.test_volume_creation()

    def test_volume_creation(self):
        uri = 'unix+tcp:{path}?export=image&timeout=1&reconn-timeout=1'.format(path=self.socket_path)
        return self.cleanup.enter_context(self.createVolume(backend='nbd', storage=uri, read_only='true'))

    def test_io_timeout(self, vol):
        with freezer([self.nbd_pid] + subpids(self.nbd_pid)):
            start = time.time()
            ExpectException(readall, OSError, os.path.join(vol.path, 'foo'))
            duration = time.time() - start
            ExpectLe(duration, 10)

    def test_reconnect_read(self, vol):
        start = time.time()
        while True:
            ExpectLe(time.time() - start, 5)
            with open(os.path.join(vol.path, 'foo')) as f:
                try:
                    ExpectEq(f.read(), 'bar')
                    break
                except OSError:
                    time.sleep(0.1)


with contextlib.ExitStack() as cleanup:
    test = TestNbdVolume(cleanup)
    test.setup()
    test.run()
