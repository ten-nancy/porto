#!/usr/bin/python3

from test_common import *
import porto
import os

AsAlice()

c = porto.Connection(timeout=10)

a = c.Run("a", symlink="tmp: /tmp; a/b: c/d")
ExpectEq(a['symlink'], "a/b: c/d; tmp: /tmp; ")
ExpectEq(a['symlink[tmp]'], "/tmp")
ExpectEq(os.readlink(a['cwd'] + "/tmp"), "../../../tmp")
ExpectEq(os.readlink(a['cwd'] + "/a/b"), "../c/d")
ExpectEq(os.stat(a['cwd'] + "/a").st_uid, alice_uid)
ExpectEq(os.stat(a['cwd'] + "/a").st_gid, alice_gid)
ExpectEq(os.stat(a['cwd'] + "/a").st_mode & 0o777, 0o775)
ExpectEq(os.lstat(a['cwd'] + "/a/b").st_uid, alice_uid)

a["symlink[tmp]"] = "foo"
ExpectEq(os.readlink(a['cwd'] + "/tmp"), "foo")
ExpectEq(a['symlink'], "a/b: c/d; tmp: foo; ")

a["symlink[tmp]"] = ""
ExpectEq(os.path.lexists(a['cwd'] + "/tmp"), False)
ExpectEq(a['symlink'], "a/b: c/d; ")

a.SetSymlink("tmp", "/foo")
ExpectEq(a['symlink'], "a/b: c/d; tmp: /foo; ")

ExpectException(a.SetProperty, porto.exceptions.Permission, "symlink[/test]", "foo")

v = c.CreateVolume(layers=["ubuntu-noble"], containers="a")
a.Stop()
a['symlink'] = "/foo/bar//../bar/baz: /xxx"
a['symlink[a]'] = 'b'
a['root'] = v.path
a.Start()
ExpectEq(a['symlink'], "/foo/bar/baz: /xxx; a: b; ")
ExpectEq(os.readlink(a['root'] + "/a"), "b")
ExpectEq(os.readlink(a['root'] + "/foo/bar/baz"), "../../xxx")

a["symlink[/test/test]"] = "foo"
ExpectEq(os.readlink(a['root'] + "/test/test"), "../foo")

a["symlink"] = ""
ExpectEq(a['symlink'], "")
ExpectEq(os.path.lexists(a['root'] + "/test/test"), False)
ExpectEq(os.path.lexists(a['root'] + "/a"), False)
ExpectEq(os.path.lexists(a['root'] + "/foo/bar/baz"), False)

a.Destroy()

m = c.Run("m")
a = c.Run("m/a", root_volume={"layers": ["ubuntu-noble"]})
a["symlink"] = "/a: /b"
ExpectEq(os.path.lexists(a['root'] + "/a"), True)
a.Destroy()
m.Destroy()


a = c.Run("a", root_volume={"layers": ["ubuntu-noble"]}, symlink="/usr/lib/tmp: /tmp", user=alice_uid, group=alice_gid)
a["symlink"] = "/usr/lib/tmp: /tmp"
ExpectEq(a["symlink"], "/usr/lib/tmp: /tmp; ")

a.Destroy()
