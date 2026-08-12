#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# ota_push.py - headless browser-push OTA for the MQTT-IO gateway.
#
# Streams a signed firmware image to the device's /fwchunk.cgi as hex-encoded
# GET chunks (seq / last / data), with totlen + totcrc on the final chunk - the
# same wire protocol the Tools web page uses.  On CC35x1 the device stages the
# bytes into the inactive PSA-FWU vendor slot and reboots into it in TRIAL; on
# TM4C it programs internal flash.  Success is the device serving a page whose
# body contains FWUPDATE_OK, after which it reboots.
#
# IMPORTANT - one connection, not thousands:  a ~1.2 MB image is ~2700 chunks.
# Opening a fresh TCP connection per chunk overwhelms the CC35x1's small lwIP
# stack (8 TCP PCBs, 60 KB heap): the heap fills with TIME_WAIT buffers and the
# httpd stalls after ~180 connections.  So we hold a SINGLE HTTP/1.1 keep-alive
# connection open for the whole upload (the device enables keep-alive and serves
# the tiny "/otaack.txt" ACK as a persistent response).  Browsers pool
# connections the same way, so the Tools page benefits automatically.
#
# Usage:
#   python ota_push.py <host> <image.bin> [--port 80]
#   e.g.  python ota_push.py 192.168.1.139 \
#             ../../../mqtt_io_cc35x1/Debug/toolbox/primary_vendor_image.sign.bin
# -----------------------------------------------------------------------------
import argparse
import binascii
import http.client
import sys
import time

FWCHUNK_DEFAULT = 448   # binary bytes per GET; matches tools.shtml FWCHUNK


POST_CHUNK_DEFAULT = 4096   # binary bytes per POST body; must be <= device OTA_POST_CHUNK_MAX


def post_upload(host, port, data, timeout, chunk=POST_CHUNK_DEFAULT):
    """Upload the signed image as many small BINARY POSTs to /fwupload?seq=N&last=L
    over ONE keep-alive connection - the CC35x1 fast path.  A single large POST body
    stalls this stack after one TCP window (8760 B) because the device sends nothing
    back mid-body, so window updates never flush; chunking makes every POST fit a
    window and its per-chunk reply slides the window for the next (same pattern
    hex-GET uses, but binary - no 2x hex - and ~10x larger chunks).  Success = the
    final chunk's response body contains FWUPDATE_OK.  Returns a process exit code."""
    total = len(data)
    nchunks = (total + chunk - 1) // chunk
    print("POST %d bytes to http://%s:%d/fwupload in %d binary chunks (keep-alive)"
          % (total, host, port, nchunks))

    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    off = 0
    seq = 0
    t0 = time.time()
    while off < total:
        n = min(chunk, total - off)
        last = 1 if (off + n >= total) else 0
        body = data[off:off + n]
        path = "/fwupload?seq=%d&last=%d" % (seq, last)
        # The final chunk triggers psa_fwu_finish + psa_fwu_install (hashing the
        # whole image) - allow much longer for that one response.
        if last:
            conn.timeout = max(timeout, 120.0)

        ok = False
        reason = "error"
        for attempt in range(6):
            try:
                conn.request("POST", path, body=body,
                             headers={"Content-Type": "application/octet-stream",
                                      "Content-Length": str(n),
                                      "Connection": "keep-alive"})
                resp = conn.getresponse()
                rbody = resp.read()         # drain so the connection can be reused
                if resp.status == 200:
                    if not last:
                        ok = True
                        break
                    if b"FWUPDATE_OK" in rbody:
                        ok = True
                        break
                    print("\ndevice REJECTED the image on the final chunk "
                          "(integrity/PSA check failed).")
                    conn.close()
                    return 1
                reason = "HTTP %d" % resp.status
            except Exception as e:          # noqa: BLE001 - reconnect once and retry
                reason = str(e) or "error"
                try:
                    conn.close()
                except Exception:           # noqa: BLE001
                    pass
                conn = http.client.HTTPConnection(host, port, timeout=timeout)
            time.sleep(0.2 + 0.3 * attempt)

        if not ok:
            tail = (" (device may already be rebooting - check the running version "
                    "before retrying)") if last else ""
            print("\nfailed at chunk %d/%d: %s%s" % (seq, nchunks, reason, tail))
            conn.close()
            return 1

        off += n
        seq += 1
        if seq % 20 == 0 or off >= total:
            dt = time.time() - t0
            print("\r  %d/%d chunks (%.1f%%, %.0f KB/s)"
                  % (seq, nchunks, off * 100.0 / total,
                     off / 1024.0 / max(0.001, dt)), end="")
            sys.stdout.flush()

    conn.close()
    dt = time.time() - t0
    print("\nupload verified in %.1fs (%.0f KB/s) - device rebooting into the new image."
          % (dt, total / 1024.0 / max(0.001, dt)))
    return 0


class Device:
    """A single reusable HTTP/1.1 keep-alive connection to the device."""

    def __init__(self, host, port, timeout):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.conn = None

    def _connect(self):
        if self.conn is not None:
            self.conn.close()
        self.conn = http.client.HTTPConnection(self.host, self.port,
                                               timeout=self.timeout)

    def get(self, path, timeout=None):
        """GET path on the persistent connection, reconnecting once if the
        server closed it. Returns (status, body). Raises on hard failure."""
        for attempt in range(2):
            try:
                if self.conn is None:
                    self._connect()
                if timeout is not None:
                    self.conn.sock.settimeout(timeout) if self.conn.sock else None
                self.conn.request("GET", path, headers={"Connection": "keep-alive"})
                resp = self.conn.getresponse()
                body = resp.read()          # drain so the connection can be reused
                return resp.status, body
            except Exception:
                # Stale/closed keep-alive socket: reconnect once and retry.
                self.conn = None
                if attempt == 1:
                    raise
        raise RuntimeError("unreachable")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="device IP or hostname, e.g. 192.168.1.139")
    ap.add_argument("image", help="signed firmware .bin to upload")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--chunk", type=int, default=FWCHUNK_DEFAULT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--post", action="store_true",
                    help="stream the image as a single binary POST to /fwupload "
                         "(CC35x1 fast path); default is the legacy hex-GET chunk loop")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        data = f.read()
    total = len(data)
    if total == 0:
        sys.exit("image is empty")

    # Fast path: one streaming POST (device validates the signed manifest itself).
    if args.post:
        sys.exit(post_upload(args.host, args.port, data, max(args.timeout, 300.0)))

    crc = binascii.crc32(data) & 0xFFFFFFFF
    nchunks = (total + args.chunk - 1) // args.chunk
    print("uploading %s (%d bytes, crc=0x%08x) to %s in %d chunks (keep-alive)"
          % (args.image, total, crc, args.host, nchunks))

    dev = Device(args.host, args.port, args.timeout)
    seq = 0
    off = 0
    t0 = time.time()
    while off < total:
        n = min(args.chunk, total - off)
        last = 1 if (off + n >= total) else 0
        path = "/fwchunk.cgi?seq=%d&last=%d&data=%s" % (seq, last,
                                                        data[off:off + n].hex())
        if last:
            path += "&totlen=%d&totcrc=%08x" % (total, crc)
        # The final chunk triggers psa_fwu_finish + psa_fwu_install (hashing the
        # whole image) - allow much longer for the response.
        req_timeout = max(args.timeout, 120.0) if last else args.timeout

        ok = False
        reason = "error"
        for attempt in range(6):
            try:
                status, body = dev.get(path, timeout=req_timeout)
                if status == 200:
                    if not last:
                        ok = True
                        break
                    if b"FWUPDATE_OK" in body:
                        ok = True
                        break
                    sys.exit("\ndevice REJECTED the image (integrity/PSA check "
                             "failed) on the final chunk")
                reason = "HTTP %d" % status
            except Exception as e:      # noqa: BLE001 - report and retry
                reason = str(e) or "error"
            time.sleep(0.3 + 0.5 * attempt)

        if not ok:
            tail = " (device may already be rebooting - check the running " \
                   "version before retrying)" if last else ""
            sys.exit("\nfailed at chunk %d/%d: %s%s" % (seq, nchunks, reason, tail))

        off += n
        seq += 1
        if seq % 100 == 0 or off >= total:
            rate = seq / max(0.001, time.time() - t0)
            print("\r  %d/%d chunks (%.1f%%, %.0f chunks/s)"
                  % (seq, nchunks, off * 100.0 / total, rate), end="")
            sys.stdout.flush()

    print("\nupload verified in %.1fs - device rebooting into the new image."
          % (time.time() - t0))


if __name__ == "__main__":
    main()
