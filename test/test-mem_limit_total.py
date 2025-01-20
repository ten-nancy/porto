import porto
from test_common import *
import os

USE_CGROUP2 = GetUseCgroup2()
print("use {} hierarchy".format("cgroup2" if USE_CGROUP2 else "cgroup1"))

conn = porto.Connection(timeout=30)

def get_kwargs(memory_limit, anon_limit):
    kwargs = {"memory_limit": memory_limit}
    if not USE_CGROUP2:
       kwargs["anon_limit"] = anon_limit
    return kwargs

a = conn.Run("a", **get_kwargs("10M", "5M"))
ExpectProp(a, 'memory_limit', "10485760")
ExpectProp(a, 'memory_limit_total', "10485760")
if not USE_CGROUP2:
    ExpectProp(a, 'anon_limit', "5242880")
    ExpectProp(a, 'anon_limit_total', "5242880")

b = conn.Run("a/b")
ExpectProp(b, 'memory_limit', "0")
ExpectProp(b, 'memory_limit_total', "10485760")
if not USE_CGROUP2:
    ExpectProp(b, 'anon_limit', "0")
    ExpectProp(b, 'anon_limit_total', "5242880")

c = conn.Run("a/c", **get_kwargs("2M", "1M"))
ExpectProp(c, 'memory_limit', "2097152")
ExpectProp(c, 'memory_limit_total', "2097152")
if not USE_CGROUP2:
    ExpectProp(c, 'anon_limit', "1048576")
    ExpectProp(c, 'anon_limit_total', "1048576")

d = conn.Run("a/d", **get_kwargs("20M", "10M"))
ExpectProp(d, 'memory_limit', "20971520")
ExpectProp(d, 'memory_limit_total', "10485760")
if not USE_CGROUP2:
    ExpectProp(d, 'anon_limit', "10485760")
    ExpectProp(d, 'anon_limit_total', "5242880")


e = conn.Run("e")

total = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
def_limit = max(total - (2<<30), total * 3/4)
def_anon = max(def_limit - (16<<20), def_limit * 3/4)

ExpectProp(e, 'memory_limit', '0')
ExpectProp(e, 'memory_limit_total', str(total))
if not USE_CGROUP2:
    ExpectProp(e, 'anon_limit', '0')
    ExpectProp(e, 'anon_limit_total', str(total))
