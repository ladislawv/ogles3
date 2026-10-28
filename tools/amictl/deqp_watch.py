#!/usr/bin/env python3
"""Runs a dEQP package's Run-* script on the A1200 and watches it.

  usage: deqp_watch.py PACKAGE-DIR "KEY=VALUE ..." [--every S] [--answer S] [--give-up S] [--timeout S]

As amictl's runpkg (the package uploaded, its results/ fetched into
tests/a1200/results/<package>-<time>/), but the script runs with PROGRESS=1:
dEQP appends each case's start ("S <case>") and end ("E <status>", "T
<status>" for a crash or a timeout) to RAM:deqp-progress.txt
(ports/deqp/patches/0003-amiga-progress-file.patch) and the library logs to
RAM:ogles2.log. Every --every seconds (15) this asks the station whether it
is alive (HELLO: its start counter, which a reset raises), reads both files
from where it last stopped (amictld 1.4's GETFROM) and prints one line:
the cases done and their counts, the last case finished and its status, the
case running and for how long, and the library's new lines (those that are
not compile and link reports). Both files are kept as they arrive in the
results directory (live-progress.txt, live-ogles2.log): after a crash they
hold what the A1200 had written up to the last report.

No answer for --answer seconds (35): the A1200 hangs or resets; reported with
the case that was running, and given up after --give-up seconds (300) without
an answer. A raised start counter: the A1200 restarted. Exit status 0 when
the script ended, 3 when the A1200 hung or reset, 4 at --timeout.

The station serves one client at a time: nothing else may talk to it while
this runs (it never blocks it for long: WAIT with a short timeout).
"""
import argparse, datetime, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import amictl

PROGRESS = "RAM:deqp-progress.txt"
LOG = "RAM:ogles2.log"
QUIET_LOG = re.compile(r"^\s*\d+ (compiler: |glLinkProgram: |draw: OGLES2Cache)")


class Link:
    """A connection to the station, made again after a failure."""

    def __init__(self, answer):
        self.answer = answer
        self.st = None
        self.token = amictl.config()["token"]

    def get(self):
        if self.st is None:
            self.st = amictl.Station(timeout=self.answer)
        return self.st

    def drop(self):
        if self.st is not None:
            try:
                self.st.close()
            except Exception:
                pass
        self.st = None

    def starts(self):
        """The station's start counter, from a new connection's greeting (a
        second HELLO on one connection answers nothing)."""
        self.drop()
        m = re.search(r"starts (\d+)", self.get().hello_text)
        return int(m.group(1)) if m else -1


def read_more(st, remote, offset):
    """New bytes of a growing file; b"" when it is not there (yet)."""
    try:
        size, data = st.get_from(remote, offset)
    except amictl.StationError as e:
        if e.status == 3:  # not there yet
            return b""
        raise
    if size < offset:  # started again
        return b""
    return data


class Progress:
    def __init__(self):
        self.buf = b""
        self.counts = {}
        self.done = 0
        self.last = None      # (case, status)
        self.running = None   # case
        self.since = None

    def feed(self, data):
        self.buf += data
        lines = self.buf.split(b"\n")
        self.buf = lines.pop()
        for raw in lines:
            line = raw.decode("latin-1").rstrip("\r")
            kind, _, text = line.partition(" ")
            if kind == "S":
                self.running = text
                self.since = time.time()
            elif kind in ("E", "T"):
                status = text if kind == "E" else "Terminated:" + text
                self.counts[status] = self.counts.get(status, 0) + 1
                self.done += 1
                self.last = (self.running, status)
                self.running = None

    def summary(self):
        counts = ", ".join("%d %s" % (n, s) for s, n in sorted(self.counts.items()))
        last = "%s %s" % self.last if self.last else "-"
        run = ("%s (%ds)" % (self.running, time.time() - self.since)) if self.running else "-"
        return "%d done (%s); last: %s; running: %s" % (self.done, counts or "none", last, run)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("package")
    ap.add_argument("args", nargs="?", default="")
    ap.add_argument("--every", type=float, default=15)
    ap.add_argument("--answer", type=float, default=35)
    ap.add_argument("--give-up", type=float, default=300)
    ap.add_argument("--timeout", type=float, default=6 * 3600)
    a = ap.parse_args(argv)

    pkgdir = os.path.abspath(a.package)
    name = os.path.basename(pkgdir.rstrip("/\\"))
    scripts = [f for f in os.listdir(pkgdir) if f.startswith("Run-")]
    if len(scripts) != 1:
        raise SystemExit("deqp_watch: %s needs exactly one Run-* script" % pkgdir)
    link = Link(a.answer)
    st = link.get()
    rc, out, _ = st.run("CD", "", 30)
    workdir = out.decode("latin-1").strip().splitlines()[-1] if rc == 0 and out.strip() else ""
    if ":" not in workdir:
        raise SystemExit("deqp_watch: cannot find the server's work directory (CD said %r)" % out)
    remote = amictl.join(workdir, name)
    print("uploading %s -> %s: %d files" % (pkgdir, remote, amictl.put_tree(st, pkgdir, remote)), flush=True)
    st.run('Delete "%s" ALL QUIET FORCE >NIL:' % amictl.join(remote, "results"), "", 60)
    st.run("Avail FLUSH >NIL:", "", 60)
    dest = os.path.join(amictl.REPO, "tests", "a1200", "results",
                        "%s-%s" % (name, datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    os.makedirs(os.path.join(dest, "results"), exist_ok=True)
    live_progress = open(os.path.join(dest, "results", "live-progress.txt"), "ab")
    live_log = open(os.path.join(dest, "results", "live-ogles2.log"), "ab")

    starts0 = link.starts()
    st = link.get()
    command =("Execute %s %s PROGRESS=1" % (scripts[0], a.args)).strip()
    job = st.start(command, remote)
    print("%s started: %s (station start %d)" % (time.strftime("%H:%M:%S"), command, starts0), flush=True)

    prog = Progress()
    poff = loff = 0
    t0 = last_answer = time.time()
    result, rc, out = 0, None, b""
    warned = False
    while True:
        time.sleep(a.every)
        new_log = []
        try:
            starts = link.starts()  # a new connection each time
            st = link.get()
            if starts != starts0:
                print("%s THE A1200 RESTARTED (station start %d, was %d); %s" %
                      (time.strftime("%H:%M:%S"), starts, starts0, prog.summary()), flush=True)
                result = 3
                break
            data = read_more(st, PROGRESS, poff)
            poff += len(data)
            live_progress.write(data)
            live_progress.flush()
            prog.feed(data)
            data = read_more(st, LOG, loff)
            loff += len(data)
            live_log.write(data)
            live_log.flush()
            new_log = [l for l in data.decode("latin-1", "replace").splitlines() if l.strip()]
            done, rc, out = st.wait(job, 0.2)
            last_answer = time.time()
            warned = False
        except Exception as e:  # no answer, a dropped connection
            link.drop()
            quiet = time.time() - last_answer
            if quiet >= a.answer:
                print("%s NO ANSWER for %ds (%s); %s" % (time.strftime("%H:%M:%S"), quiet,
                                                         type(e).__name__, prog.summary()), flush=True)
                warned = True
            if quiet >= a.give_up:
                print("given up: the A1200 hangs or reset while running %s" % (prog.running or "no case"),
                      flush=True)
                result = 3
                break
            continue
        notable = [l for l in new_log if not QUIET_LOG.match(l)]
        print("%s %s; log +%d%s" % (time.strftime("%H:%M:%S"), prog.summary(), len(new_log),
                                    "".join("\n    " + l for l in notable[-5:])), flush=True)
        if done:
            break
        if time.time() - t0 > a.timeout:
            print("timed out after %ds" % (time.time() - t0), flush=True)
            st.call("ABORT", str(job))
            result = 4
            break
    live_progress.close()
    live_log.close()
    if result == 0:
        print(out.decode("latin-1", "replace")[-2000:], end="")
        print("%s: rc=%s, %.1f s" % (scripts[0], rc, time.time() - t0))
        n = amictl.get_tree(link.get(), remote + "/results", os.path.join(dest, "results"))
        print("results: %d files -> %s" % (n, dest))
    else:
        print("live copies: %s" % os.path.join(dest, "results"))
    return result


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
