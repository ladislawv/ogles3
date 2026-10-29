"""cmpprof.py [--per CALLS_A CALLS_B] A_PROF... -- B_PROF...: libprof.py outputs of runs A
and B summed per side (averaged over each side's runs), by function, ordered by B - A.
With --per, each side's samples per 1000 calls of a test (drawbench: a sample is a
millisecond, so that is microseconds a call).
"""
import collections
import sys

args = sys.argv[1:]
per = None
if args[:1] == ["--per"]:
    per = (float(args[1]), float(args[2]))
    args = args[3:]
k = args.index("--")


def load(files):
    c = collections.Counter()
    for f in files:
        for line in open(f).readlines()[1:]:
            n, name = line.split(None, 1)
            c[name.strip()] += int(n)
    return c, len(files)


a, na = load(args[:k])
b, nb = load(args[k + 1:])
fa = 1000.0 / per[0] if per else 1.0 / na
fb = 1000.0 / per[1] if per else 1.0 / nb
print("%8s %8s %8s  function (%s)" % ("A", "B", "B-A", "us a call" if per else "samples a run"))
print("%8.2f %8.2f %+8.2f  (in the library)" % (sum(a.values()) * fa, sum(b.values()) * fb,
                                                sum(b.values()) * fb - sum(a.values()) * fa))
rows = sorted(set(a) | set(b), key=lambda n: -(b[n] * fb - a[n] * fa))
for n in rows[:30]:
    print("%8.2f %8.2f %+8.2f  %s" % (a[n] * fa, b[n] * fb, b[n] * fb - a[n] * fa, n))
print("...")
for n in rows[-8:]:
    print("%8.2f %8.2f %+8.2f  %s" % (a[n] * fa, b[n] * fb, b[n] * fb - a[n] * fa, n))
