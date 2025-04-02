#!/usr/bin/python3

from test_common import *
import porto
import os

TEST_PATH_1 = "/tmp/test1"
TEST_PATH_2 = "/tmp/test2"
TEST_PATH_3 = "/tmp/test3"
CT_ROOT = TEST_PATH_3 + "/1/root"

c = porto.Connection(timeout=30)

all_volumes = [
    TEST_PATH_1 + "/volume/1",
    TEST_PATH_1 + "/kek/1",
    TEST_PATH_1 + "/kek/2",
    TEST_PATH_1 + "/kek/3",
    TEST_PATH_1 + "/kek/lol/1",
    TEST_PATH_2 + "/volume/1",
    TEST_PATH_2 + "/kek/1",
    TEST_PATH_2 + "/kek/2",
    TEST_PATH_2 + "/kek/3",
    TEST_PATH_2 + "/kek/lol/1",
]

ct_volumes = [
    # Test listing volumes for container
    CT_ROOT,
    TEST_PATH_3 + "/1/lol",
    TEST_PATH_3 + "/1/root-",
    TEST_PATH_3 + "/1/root/kek",
]

mask_and_needed_output = [
    ["/*/***", [                      # incorrect using of wildcard

    ]],

    [ "",
        all_volumes + ct_volumes,
    ],

    [ "***",
        all_volumes + ct_volumes,
    ],

    [ TEST_PATH_1 + "/volume/1", [
        TEST_PATH_1 + "/volume/1",
    ]],

    ["***/volume/1", [
        TEST_PATH_1 + "/volume/1",
        TEST_PATH_2 + "/volume/1",
    ]],

    [TEST_PATH_1 + "/***", [
        TEST_PATH_1 + "/volume/1",
        TEST_PATH_1 + "/kek/1",
        TEST_PATH_1 + "/kek/2",
        TEST_PATH_1 + "/kek/3",
        TEST_PATH_1 + "/kek/lol/1",
    ]],

    ["/*/test1/kek/*", [
        TEST_PATH_1 + "/kek/1",
        TEST_PATH_1 + "/kek/2",
        TEST_PATH_1 + "/kek/3",
    ]],
]

a = None

try:
    for volume in all_volumes + ct_volumes:
        os.makedirs(volume)
        c.CreateVolume(path=volume)

    for [mask, needed_output] in mask_and_needed_output:
        needed_output = sorted(needed_output)
        output = sorted([str(v) for v in c.ListVolumes(mask)])
        ExpectEq(needed_output, output)

    a = c.Create(name='foo')
    a.SetProperty('root', CT_ROOT)
    ExpectEq(
        set(map(str, c.ListVolumes(container=a))),
        {'/', '/kek'},
    )
    a.SetProperty('root', os.path.dirname(CT_ROOT))
    ExpectEq(
        set(map(str, c.ListVolumes(container=a))),
        {'/root', '/root/kek', '/root-', '/lol'},
    )
    a.SetProperty('root', CT_ROOT + "/foobar")
    ExpectEq(
        set(map(str, c.ListVolumes(container=a))),
        set(),
    )
finally:
    for volume in all_volumes + ct_volumes[::-1]:
        c.UnlinkVolume(volume)
        os.removedirs(volume)
    if a:
        a.Destroy()
