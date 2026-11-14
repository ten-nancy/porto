import porto
from porto import rpc_pb2
from test_common import CreateContainer, ExpectEq, ExpectException


def test_property(conn, name):
    with CreateContainer(conn, 'test-unsupported-property') as ct:
        ExpectEq(ct[name], False)
        ct[name] = False
        for mode in ('app', 'os', 'host', 'job'):
            ct['virt_mode'] = mode
            ExpectException(ct.SetProperty, porto.exceptions.NotSupported, name, True)
            ExpectEq(ct[name], False)


def test_spec(conn, name):
    with CreateContainer(conn, 'test-unsupported-spec') as ct:
        ExpectException(ct.LoadSpec, porto.exceptions.NotSupported,
                        rpc_pb2.TContainerSpec(**{name: True}))
        ct.LoadSpec(rpc_pb2.TContainerSpec(**{name: False}))
        ExpectEq(getattr(ct.Dump().spec, name), False)
        ExpectEq(ct[name], False)


def test_fuse_mode(conn):
    with CreateContainer(conn, 'test-unsupported-fuse-mode') as ct:
        ct['virt_mode'] = 'os'
        ExpectException(ct.SetProperty, porto.exceptions.NotSupported, 'virt_mode', 'fuse')
        ExpectException(ct.LoadSpec, porto.exceptions.NotSupported,
                        rpc_pb2.TContainerSpec(virt_mode='fuse'))
        ExpectEq(ct['virt_mode'], 'os')


if __name__ == '__main__':
    conn = porto.Connection(timeout=30)
    for name in ('userns', 'unshare_on_exec', 'enable_fuse'):
        test_property(conn, name)
        test_spec(conn, name)
    test_fuse_mode(conn)
