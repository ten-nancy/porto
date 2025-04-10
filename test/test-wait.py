from test_common import *
import porto
import time

c = porto.Connection(timeout=30)

# stopped
a = c.Create("a")
ExpectEq(c.WaitContainers(["a"]), "a")
a.Destroy()

# dead
a = c.Run("a", command="true")
ExpectEq(c.WaitContainers(["a"]), "a")
a.Destroy()

# non-block
a = c.Run("a", command="sleep 1000")
ExpectEq(Catch(c.WaitContainers, ["a"], timeout=0), porto.exceptions.WaitContainerTimeout)
a.Destroy()

# timeout
a = c.Run("a", command="sleep 1000")
ExpectEq(Catch(c.WaitContainers, ["a"], timeout=0.1), porto.exceptions.WaitContainerTimeout)
a.Destroy()

# setup async
events = []
def wait_event(name, state, when, **kwargs):
    ExpectEq((name, state), events.pop(0))

c.AsyncWait(["a"], wait_event)

# full async cycle
events=[('a', 'stopped'), ('a', 'starting'), ('a', 'running'), ('a', 'stopping'), ('a', 'stopped'), ('a', 'destroyed')]
a = c.Run("a", command="tail -f /dev/null")
time.sleep(0.5)
a.Destroy()
ExpectEq(events, [])

# restore async at reconnect
events=[('a', 'stopped'), ('a', 'starting'), ('a', 'running'), ('a', 'running'), ('a', 'stopping'), ('a', 'stopped'), ('a', 'destroyed')]
a = c.Run("a", weak=False, command="tail -f /dev/null")
time.sleep(0.5)
ReloadPortod()
time.sleep(0.5)
a.Destroy()
ExpectEq(events, [])
