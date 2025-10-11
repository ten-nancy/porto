from multiprocessing.pool import ThreadPool
import os
import porto
import tempfile

from test_common import CreateVolume, ExpectEq

def test_mkdir_race(source):
    source_st = os.stat(source)
    N = 4
    conns = [porto.Connection() for _ in range(N)]
    pool = ThreadPool(processes=N)

    for i in range(128):
        with CreateVolume(conns[0]) as main_vol:
            try:
                vols = pool.map(
                    lambda i: conns[i].NewVolume({
                        "backend": "bind",
                        "storage": source,
                        "links": [{
                            "container": "self",
                            "target": os.path.join(main_vol.path, "foo/bar/baz", 'file{}'.format(i))
                        }],
                        "place": main_vol.path,
                    }),
                    range(N)
                )
                for vol in vols:
                    [link] = vol['links']
                    st = os.stat(link['target'])
                    ExpectEq(st.st_dev, source_st.st_dev)
                    ExpectEq(st.st_ino, source_st.st_ino)
            finally:
                main_vol.Unlink()


if __name__ == '__main__':
    with tempfile.NamedTemporaryFile() as tmp:
        test_mkdir_race(tmp.name)
