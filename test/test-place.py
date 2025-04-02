import porto
from test_common import *
import shutil

c = porto.Connection(timeout=30)
print(porto.__file__)

a = c.Run('a', place_limit="/place: 1G")
ExpectEq(a['place_usage'], "")
ExpectEq(a['place_limit'], "/place: 1073741824")

v = c.CreateVolume(space_limit='1G', owner_container='a')
ExpectEq(a['place_usage'], "/place: 1073741824; total: 1073741824")
ExpectEq(Catch(c.CreateVolume, space_limit='1G', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, space_limit='0', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, owner_container='a'), porto.exceptions.ResourceNotAvailable)
v.Destroy()

ExpectEq(a['place_usage'], "/place: 0; total: 0")
ExpectEq(Catch(c.CreateVolume, owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, space_limit='0', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, backend='plain', owner_container='a'), porto.exceptions.ResourceNotAvailable)
ExpectEq(Catch(c.CreateVolume, space_limit='2G', owner_container='a'), porto.exceptions.ResourceNotAvailable)

v = c.CreateVolume(space_limit='1M', owner_container='a')
ExpectEq(a['place_usage'], "/place: 1048576; total: 1048576")
v.Destroy()

a.Destroy()


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


vol = c.CreateVolume()


os.mkdir(os.path.join(vol.path, 'porto_storage'))
c.CleanupPlace(vol.path)
assert os.path.exists(os.path.join(vol.path, 'porto_storage'))
os.mkdir(os.path.join(vol.path, 'porto_storage', '_remove_asdasd'))

c.CleanupPlace(vol.path)
assert os.path.exists(os.path.join(vol.path, 'porto_storage'))
assert not os.path.exists(os.path.join(vol.path, 'porto_storage', '_remove_asdasd'))
