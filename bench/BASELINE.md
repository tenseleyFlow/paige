# Benchmark Baseline

Local smoke baseline captured on 2026-05-30 with:

```sh
make bench-smoke
```

System context:

```text
FreeBSD dorado 15.0-RELEASE FreeBSD 15.0-RELEASE releng/15.0-n280995-7aedc8de6446 GENERIC amd64
```

`rss_max` is `getrusage(2)` `ru_maxrss`; units vary by OS. These numbers are a
script-health baseline, not a marketing comparison.

```text
run=1 bench mode=first name=paige elapsed_ms=4.939 first_ms=4.939 rss_max=0 pty_bytes=1682 pty_reads=2 status=ok
run=1 bench mode=jump name=paige elapsed_ms=0.860 first_ms=6.277 rss_max=10300 pty_bytes=3276 pty_reads=4 status=ok
run=1 bench mode=search name=paige-late elapsed_ms=50.016 first_ms=4.935 rss_max=11016 pty_bytes=11557 pty_reads=22 status=ok
run=1 bench mode=search name=paige-nohit elapsed_ms=80.913 first_ms=4.910 rss_max=11016 pty_bytes=23515 pty_reads=25 status=ok
run=1 bench mode=first name=paige-huge-wrap elapsed_ms=6.109 first_ms=6.109 rss_max=14564 pty_bytes=2115 pty_reads=4 status=ok
run=1 bench mode=first name=paige-huge-chop elapsed_ms=6.735 first_ms=6.735 rss_max=9436 pty_bytes=391 pty_reads=3 status=ok
run=1 bench mode=first name=less elapsed_ms=1.739 first_ms=1.739 rss_max=0 pty_bytes=1568 pty_reads=4 status=ok
run=1 bench mode=jump name=less elapsed_ms=4.428 first_ms=1.715 rss_max=2944 pty_bytes=2999 pty_reads=8 status=ok
run=1 bench mode=search name=less-late elapsed_ms=13.376 first_ms=1.676 rss_max=3572 pty_bytes=5330 pty_reads=31 status=ok
run=1 bench mode=first name=less-huge elapsed_ms=1.671 first_ms=1.671 rss_max=0 pty_bytes=1908 pty_reads=4 status=ok
```
