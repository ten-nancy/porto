from test_common import *


conn = porto.Connection(timeout=30)
name = 'test-cgroupfs-hierarchy'
modes = ('none', 'ro', 'rw')


def CheckHierarchy():
    for parent_mode in modes:
        with RunContainer(conn, name, cgroupfs=parent_mode) as parent:
            for child_mode in modes:
                with CreateContainer(conn, name + '/child') as child:
                    child['cgroupfs'] = child_mode
                    if modes.index(child_mode) > modes.index(parent_mode):
                        ExpectException(child.Start, porto.exceptions.PermissionError)
                        ExpectEq(child['state'], 'stopped')
                    else:
                        child.Start()
                        ExpectEq(child['state'], 'meta')
                        ExpectEq(child['cgroupfs'], child_mode)
                print('cgroupfs: {} -> {} checked'.format(parent_mode, child_mode))


def CheckOsDefault():
    for parent_mode in modes:
        with RunContainer(conn, name, cgroupfs=parent_mode,
                          virt_mode='os', command='sleep 300') as parent:
            with CreateContainer(conn, name + '/child') as child:
                child['virt_mode'] = 'os'
                child['command'] = 'sleep 300'
                if parent_mode == 'rw':
                    child.Start()
                    ExpectEq(child['state'], 'running')
                    ExpectEq(child['cgroupfs'], 'rw')
                else:
                    ExpectException(child.Start, porto.exceptions.PermissionError)
                    ExpectEq(child['state'], 'stopped')
            print('cgroupfs: {} -> OS default checked'.format(parent_mode))


try:
    ConfigurePortod(name, """
    container {
        enable_rw_cgroupfs: true
        use_os_mode_cgroupns: false
    }""")

    CheckHierarchy()
    CheckOsDefault()

    # Recheck the current parent policy even if the child was configured first.
    with CreateContainer(conn, name) as parent:
        parent['cgroupfs'] = 'rw'
        with CreateContainer(conn, name + '/child') as child:
            child['cgroupfs'] = 'rw'
            parent['cgroupfs'] = 'ro'
            parent.Start()
            ExpectException(child.Start, porto.exceptions.PermissionError)
            ExpectEq(child['state'], 'stopped')
            parent.Stop()
            parent['cgroupfs'] = 'rw'
            parent.Start()
            child.Start()
            ExpectEq(child['state'], 'meta')

    ConfigurePortod(name, """
    container {
        enable_rw_cgroupfs: false
        use_os_mode_cgroupns: true
    }""")

    CheckOsDefault()

finally:
    ConfigurePortod(name, '')
