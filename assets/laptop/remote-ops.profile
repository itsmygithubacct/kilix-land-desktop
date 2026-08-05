# Kilix laptop profile: local shell plus a remote ssh tab.
# Replace the example destination with a real [user@]host of yours.
name=Remote Ops
layout=tabs
pane.1.title=local
pane.1.cwd=~
pane.2.title=server
pane.2.ssh=admin@example-host
pane.2.cwd=/var/log
pane.2.cmd=tail -f syslog
