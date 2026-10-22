# xrootd-multiuser-posc

`libXrdMultiuserPosc` is an XRootD OSS plugin that emulates POSC
(persist-on-successful-close) for servers running
[xrootd-multiuser](https://github.com/opensciencegrid/xrootd-multiuser), which
disables XRootD's own POSC (see opensciencegrid/xrootd-multiuser#74).

New files are created as `<dir>/.<name>.mu-part.<16 hex>` and renamed to the
final name when the upload completes.  Incomplete uploads (size differs from
`oss.asize`, close failed) are removed.  Uploads interrupted by a server stop
are journaled and removed, as their owner, at the next startup.

## Configuration

Load it **before** (i.e. below) the multiuser plugin:

```
ofs.osslib ++ libXrdMultiuserPosc.so
ofs.osslib ++ libXrdMultiuser.so

multiuser.posc.enable on                            # default off; follows "ofs.persist auto" if unset
multiuser.posc.journal /var/spool/xrootd/posc-journal   # mode 1733, owned by the daemon
multiuser.posc.trace info                           # all|debug|info|none
```

The server refuses to start if POSC is enabled and the journal directory is
missing, not creatable, or not mode 1733 and owned by the daemon.  If a
journal entry cannot be written at run time, that one file is written in
place (without POSC) and an error is logged.

Notes:
* Uploads without `oss.asize` (chunked HTTP PUT, xrdcp from a pipe) are
  published on close as before; only a server stop is covered for them.
* xroot third-party copies (`tpc.key`) are written in place.
* Checksum tools that crawl the namespace should skip `*.mu-part.*`.
