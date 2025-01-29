import porto, sys

l = []
for i in range(int(sys.argv[1])):
    c = porto.Connection(timeout=30);
    c.connect();
    l.append(c)

sys.stdin.read()
