import porto
from test_common import *

def test_storage_overlap():
    conn = porto.Connection()

    with contextlib.ExitStack() as cleanup:
        root = cleanup.enter_context(CreateVolume(conn))

        real_place = os.path.join(root.path, 'real_place')
        place = os.path.join(root.path, 'place')
        storage = os.path.join(root.path, 'storage')
        link = os.path.join(root.path, 'link')

        for x in [real_place, place, storage, link]:
            os.mkdir(x)

        cleanup.enter_context(CreateVolume(conn, place, backend='bind', storage=real_place))
        vol = cleanup.enter_context(CreateVolume(conn, None, place=place))

        ExpectException(lambda: cleanup.enter_context(CreateVolume(conn, real_place, backend='bind', storage=storage)),
                        porto.exceptions.InvalidPath)

        # TODO: maybe support behaviour above
        # In such case following action must succeed:
        # conn.LinkVolume(vol.path, 'self', target=link)


if __name__ == '__main__':
    test_storage_overlap()
