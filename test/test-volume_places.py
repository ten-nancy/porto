#!/usr/bin/python3

import os
import shutil
import tarfile
import traceback
import porto
from test_common import *

c = porto.Connection(timeout=30)

def Test(tmpdir, placedir):
    #Prepare dummy layer also
    f = open(tmpdir + "/file.txt", "w")
    f.write("1234567890")
    f.close()
    t = tarfile.open(name=tmpdir + "/file_layer.tar", mode="w")
    t.add(tmpdir + "/file.txt", arcname="file.txt")
    t.close()


    os.mkdir(placedir + "/porto_volumes")
    os.mkdir(placedir + "/porto_layers")
    os.mkdir(placedir + "/porto_storage")
    os.mkdir(tmpdir + "/a")

    #Finally, checking functions
    ExpectEq(Catch(c.CreateVolume, path=tmpdir + "/a", place=placedir, backend="native", layers=["ubuntu-noble"]), porto.exceptions.LayerNotFound)

    v = c.CreateVolume(path=None, layers=["ubuntu-noble"], backend="plain")
    v.Export(tmpdir + "/tmp_ubuntu_noble.tar")
    c.ImportLayer("place-ubuntu-noble", tmpdir + "/tmp_ubuntu_noble.tar", place=placedir)
    assert Catch(c.FindLayer, "place-ubuntu-noble") == porto.exceptions.LayerNotFound

    l = c.FindLayer("place-ubuntu-noble", place=placedir)

    assert l.GetPrivate() == ""
    l.SetPrivate("XXXX")
    assert l.GetPrivate() == "XXXX"

    l.Merge(tmpdir + "/file_layer.tar", private_value="YYYY")
    assert l.GetPrivate() == "YYYY"

    os.unlink(tmpdir + "/tmp_ubuntu_noble.tar")
    v.Unlink("/")

    #Should also fail because of foreign layer vise versa
    assert Catch(c.CreateVolume, path=tmpdir + "/a", backend="native", layers=["place-ubuntu-noble"]) == porto.exceptions.LayerNotFound

    #Check volume is working properly
    v = c.CreateVolume(path=tmpdir + "/a", place=placedir, backend="native", layers=["place-ubuntu-noble"])

    place_volumes = os.listdir(placedir + "/porto_volumes")
    assert len(place_volumes) == 1

    cont = c.Create("test")
    cont.SetProperty("command", "bash -c \"echo -n 789987 > /123321.txt\"")
    cont.SetProperty("root", v.path)
    cont.Start()
    cont.Wait()
    cont.Stop()

    f = open(placedir + "/porto_volumes/" + place_volumes[0] + "/native/123321.txt", "r")
    assert f.read() == "789987"

    cont.SetProperty("command", "cat /file.txt")
    cont.Start()
    cont.Wait()
    assert cont.Get(["stdout"])["stdout"] == "1234567890"
    cont.Destroy()

    #prepare more dirs
    os.mkdir(placedir + "/1")
    os.mkdir(placedir + "/1/2")
    os.mkdir(placedir + "/1/2/place1")
    os.mkdir(placedir + "/1/2/place1/porto_layers")
    os.mkdir(placedir + "/1/2/place1/porto_storage")

    #prepare test function
    def TestWildcards(container_name, place, allowed=True):
        cont = c.Create(container_name)
        cont.SetProperty("place", place)
        cont.SetProperty("command", portoctl + ' layer -P {}'.format(placedir + "/1/2/place1"))
        if allowed:
            cont.Start()
            cont.Wait()
            assert not cont.Get(["stderr"])["stderr"]
        else:
            assert Catch(cont.Start) == porto.exceptions.Permission
        cont.Destroy()

    #Check wildcard
    cont = c.Create("test")
    cont.SetProperty("place", "***")
    cont.SetProperty("command", portoctl + ' layer -L')
    cont.Start()
    cont.Wait()
    assert "SocketError" not in cont.Get(["stderr"])["stderr"]
    cont.Destroy()

    TestWildcards("test", "/place/***")
    TestWildcards("test", "/place/***/***")
    TestWildcards("test", "/place/***/***/***")

    #Wildcard allowed only at the end of the place
    TestWildcards("test", "/place/***/place1", False)
    TestWildcards("test", "***/place", False)

    #Check wildcards in container hierarchy

    #prepare tester class
    class WildcarsInHierarchyTester(object):
        def __init__(self):
            self.parent_places = []
            self.allowed_child_places = []
            self.not_allowed_child_places = []

        def AddParentPlace(self, parent_place):
            self.parent_places.append(parent_place)

        def AddChildPlace(self, child_place, allowed=True):
            if allowed:
                self.allowed_child_places.append(child_place)
            else:
                self.not_allowed_child_places.append(child_place)

        def Test(self):
           for parent_place in self.parent_places:
               cont = c.Create("parent")
               cont.SetProperty("place", parent_place)
               cont.SetProperty("command", ' sleep 5')
               cont.SetProperty("env", "PYTHONPATH=/porto/src/api/python")
               cont.Start()

               for place in self.allowed_child_places:
                   TestWildcards("parent/child", place)
               for place in self.not_allowed_child_places:
                   TestWildcards("parent/child", place, False)

               cont.Wait()
               assert not cont.Get(["stderr"])["stderr"]
               cont.Destroy()


    hierarchyTester = WildcarsInHierarchyTester()
    hierarchyTester.AddParentPlace(placedir + "/1/2/***")
    hierarchyTester.AddParentPlace(placedir + "/1/2/***/***")
    hierarchyTester.AddParentPlace(placedir + "/1/2/***/***/***")

    hierarchyTester.AddChildPlace(placedir + "/1/2/place1")
    hierarchyTester.AddChildPlace(placedir + "/1/2/***")
    hierarchyTester.AddChildPlace(placedir + "/1/2/***/***")
    hierarchyTester.AddChildPlace(placedir + "/1/2/***/***/***")
    hierarchyTester.AddChildPlace(placedir + "/1/2", False)
    hierarchyTester.AddChildPlace(placedir + "/***", False)
    hierarchyTester.AddChildPlace("***", False)
    hierarchyTester.AddChildPlace("/place/***", False)
    hierarchyTester.AddChildPlace("***/place1", False)
    hierarchyTester.AddChildPlace("/place/***/place1", False)
    hierarchyTester.Test()

    hierarchyTester = WildcarsInHierarchyTester()
    hierarchyTester.AddParentPlace("***")
    hierarchyTester.AddParentPlace(placedir + "***")
    hierarchyTester.AddParentPlace(placedir + "***/***")
    hierarchyTester.AddParentPlace(placedir + "***/***/***")

    hierarchyTester.AddChildPlace(placedir + "/1/2/place1")
    hierarchyTester.AddChildPlace(placedir + "/1/2/***")
    hierarchyTester.AddChildPlace(placedir + "/1/2/***/***")
    hierarchyTester.AddChildPlace(placedir + "/1/2/***/***/***")
    hierarchyTester.AddChildPlace(placedir + "/1/2")
    hierarchyTester.AddChildPlace(placedir + "/***")
    hierarchyTester.AddChildPlace(placedir + "/***/***")
    hierarchyTester.AddChildPlace("***/place1", False)
    hierarchyTester.AddChildPlace("/place/***/place1", False)
    hierarchyTester.Test()

    v.Export(tmpdir + "/tmp_back_ubuntu_noble.tar")

    v.Unlink()

    c.RemoveLayer("place-ubuntu-noble", place=placedir)

    assert len(os.listdir(placedir + "/porto_volumes")) == 0
    assert len(c.ListLayers(place=placedir)) == 0
    assert len(os.listdir(placedir + "/porto_layers")) == 0


try:
    with CreateVolume(c) as vol:
        placedir = os.path.join(vol.path, "place")
        os.mkdir(placedir)
        Test(vol.path, placedir)
finally:
    try:
        c.RemoveLayer("place-ubuntu-noble", place=PLACE_DIR)
    except Exception:
        pass
