import test_common

import porto


c = porto.Connection(timeout=30)
# common case
ct = c.CreateWeakContainer('test-net-props')
ct.SetProperty('net', "L3 veth")
ct.SetProperty('net_limit', 'default: 7255')
ct.SetProperty('net_rx_limit', 'default: 7255M')

ct2 = c.CreateWeakContainer('test-net-props/child')
assert str(ct2.GetProperty('net_limit_bound')) == 'default: 7255'
assert str(ct2.GetProperty('net_rx_limit_bound')) == 'default: 7607418880'


# uncommon case
ct3 = c.CreateWeakContainer('test-net-props3')
ct3.SetProperty('net', "L3 veth")
ct3.SetProperty('net_limit', 'default: 7255')
ct3.SetProperty('net_rx_limit', 'default: 7255M')

ct4 = c.CreateWeakContainer('test-net-props3/child')
ct5 = c.CreateWeakContainer('test-net-props3/child/child')
ct5.SetProperty('net', "L3 veth")
ct5.SetProperty('net_limit', 'default: 7255M')
ct5.SetProperty('net_rx_limit', 'default: 7255')

assert str(ct5.GetProperty('net_limit_bound')) == 'default: 7607418880'
assert str(ct5.GetProperty('net_rx_limit_bound')) == 'default: 7255'

assert str(ct4.GetProperty('net_limit_bound')) == 'default: 7255'
assert str(ct4.GetProperty('net_rx_limit_bound')) == 'default: 7607418880'


# very uncommon case
ct6 = c.CreateWeakContainer('test-net-props3/child/child/child')
assert str(ct6.GetProperty('net_limit_bound')) == 'default: 7607418880'
assert str(ct6.GetProperty('net_rx_limit_bound')) == 'default: 7255'
