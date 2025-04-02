#!/usr/bin/env python3

"""
Simulate a operator-robot network with multiple unreliable links.
"""

from mininet.cli import CLI
from mininet.net import Mininet
import mininet.link
import mininet.log
import mininet.node
import mininet.util

# specify link characteristics. bw is bandwidth in Mb/s and loss is in percent.
links = [
  {'bw':5, 'delay':'100ms', 'jitter':'50ms', 'loss':5, 'max_queue_size':250},
  {'bw':1, 'delay':'500ms', 'jitter':'250ms', 'loss':2.5, 'max_queue_size':250},
  {'bw':0.5, 'delay':'1000ms', 'jitter':'750ms', 'loss':1, 'max_queue_size':500}
]

mininet.log.setLogLevel('info')

net = Mininet(build=False, controller=mininet.node.RemoteController, link=mininet.link.TCLink, topo=None, xterms=False)

robot = net.addHost('robot', ip=None)
operator = net.addHost('operator', ip=None)

for i in range(len(links)):
  si = str(i)
  net.addLink(operator, robot, intfName1='operator-eth'+si, intfName2='robot-eth'+si, **links[i])

  robot.intf('robot-eth'+si).ip = '192.168.'+si+'.1'
  robot.intf('robot-eth'+si).prefixLen = 24
  robot.cmd('ip a a 192.168.'+si+'.1/24 dev robot-eth'+si)

  operator.intf('operator-eth'+si).ip = '192.168.'+si+'.2'
  operator.intf('operator-eth'+si).prefixLen = 24
  operator.cmd('ip a a 192.168.'+si+'.2/24 dev operator-eth'+si)

net.build()

robot.sendCmd('./robot.bash')
print (robot.waitOutput())

operator.sendCmd('./operator.bash')
print (operator.waitOutput())

CLI(net)

net.stop()
