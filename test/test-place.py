import porto
from test_common import *
import shutil
import tempfile

c = porto.Connection(timeout=30)

# PORTO-1121

def test_place(place, uid, gid):
    # it invokes CheckPlace()
    ExpectException(c.RemoveStorage, porto.exceptions.VolumeNotFound, "kek", place=place)

    for dir in os.listdir(place):
        if dir.startswith("porto_"):
            st = os.stat(os.path.join(place, dir))
            ExpectEq(st.st_uid, uid)
            ExpectEq(st.st_gid, gid)

DEFAULT_PLACE_PATH = "/place"
USER_PLACE_PATH = "/tmp/place"

try:
    # default place
    test_place(DEFAULT_PLACE_PATH, 0, porto_gid)

    # non-default place
    shutil.rmtree(USER_PLACE_PATH, ignore_errors=True)
    os.mkdir(USER_PLACE_PATH)
    os.chown(USER_PLACE_PATH, uid=alice_uid, gid=alice_gid)

    test_place(USER_PLACE_PATH, alice_uid, alice_gid)

except Exception as ex:
    raise ex

finally:
    shutil.rmtree(USER_PLACE_PATH, ignore_errors=True)


def ExpectVolumeNotExist(place, vol_id):
    assert not os.path.exists(os.path.join(place, 'porto_volumes', vol_id))
    assert not os.path.exists(os.path.join(place, 'porto_volumes', '_remove_' + vol_id))


with CreateVolume(c) as vol, tempfile.NamedTemporaryFile() as tmp:
    os.mkdir(os.path.join(vol.path, 'porto_storage'))
    c.CleanupPlace(vol.path)
    assert os.path.exists(os.path.join(vol.path, 'porto_storage'))

    with CreateVolume(c, place=vol.path) as vol1:
        os.mkdir(os.path.join(vol.path, 'porto_storage', '_remove_asdasd'))
        os.mkdir(os.path.join(vol.path, 'porto_volumes', vol['id']))
        os.mkdir(os.path.join(vol.path, 'porto_volumes', '123c'))
        os.mkdir(os.path.join(vol.path, 'porto_volumes', '231c'))
        os.mkdir(os.path.join(vol.path, 'porto_volumes', '231c', 'dir'))
        open(os.path.join(vol.path, 'porto_volumes', '231c', 'data'), 'a').close()
        subprocess.check_call(['mount', '--bind', tmp.name, os.path.join(vol.path, 'porto_volumes', '231c', 'data')])
        subprocess.check_call(['mount', '--bind', vol.path, os.path.join(vol.path, 'porto_volumes', '231c', 'dir')])

        c.CleanupPlace("/place")
        c.CleanupPlace(vol.path)

        assert os.path.exists(os.path.join(vol.path, 'porto_storage'))
        assert os.path.exists(os.path.join(vol.path, 'porto_volumes'))
        assert os.path.exists(os.path.join('/place', 'porto_volumes', vol['id']))
        assert os.path.exists(os.path.join(vol.path, 'porto_volumes', vol1['id']))

        ExpectVolumeNotExist(vol.path, '123c')
        ExpectVolumeNotExist(vol.path, '231c')
        ExpectVolumeNotExist(vol.path, vol['id'])
        assert not os.path.exists(os.path.join(vol.path, 'porto_storage', '_remove_asdasd'))
