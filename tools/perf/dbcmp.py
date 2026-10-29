"""dbcmp.py RESULTS_DIR: drawbench's TEST lines of a run side by side (2.txt ogles2, 3.txt
ogles3, 3es2.txt ogles3 in an ES 2.0 context), ns a call, and ogles3 / ogles2. Lines of
the runs that are not TEST lines (errors) are printed first."""
import os
import re
import sys

d = sys.argv[1]
cols = [("ogles2", "2.txt"), ("ogles3", "3.txt"), ("o3 es2", "3es2.txt")]
data = {}
order = []
for name, f in cols:
    p = os.path.join(d, f)
    if not os.path.exists(p):
        continue
    for line in open(p, errors="replace"):
        m = re.match(r"TEST \S+ (\S+)\s+(\d+) (ns|fps)", line)
        if m:
            if m.group(1) not in data:
                order.append(m.group(1))
            data.setdefault(m.group(1), {})[name] = float(m.group(2))
        elif line.startswith("TEST") and "gl_error" in line:
            if "gl_error" not in data:
                order.append("gl_error")
            data.setdefault("gl_error", {})[name] = line.split()[-1]
        elif line.strip() and not line.startswith("drawbench"):
            print(f, line.rstrip())
print("%-26s %10s %10s %10s %8s" % ("test (ns a call)", "ogles2", "ogles3", "o3 es2", "o3/o2"))
for t in order:
    v = data[t]
    a, b, c = v.get("ogles2", "-"), v.get("ogles3", "-"), v.get("o3 es2", "-")
    r = "%.2f" % (b / a) if isinstance(a, float) and isinstance(b, float) and a else ""
    f = lambda x: ("%10.0f" % x) if isinstance(x, float) else "%10s" % x
    print("%-26s %s %s %s %8s" % (t, f(a), f(b), f(c), r))
