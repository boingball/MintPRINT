#!/usr/bin/env python3
"""Minimal local IPP Print-Job sink for MintPRINT benchmarking.

This is intentionally not a printer emulator. It accepts MintPRINT's HTTP
POST, saves the submitted document, and immediately returns IPP successful-ok.
That removes physical-printer processing time from Amiga/WinUAE benchmarks.

Example:
    python tools/mock_ipp_printer.py --port 8631 --output-dir ipp-captures

Point MintPRINT at the PC's LAN address, port 8631, path /ipp/print.
"""

from __future__ import print_function

import argparse
import os
import sys
import time

try:
    from http.server import BaseHTTPRequestHandler, HTTPServer
except ImportError:  # Python 2 fallback, though Python 3 is recommended.
    from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer


IPP_OK = b"\x01\x01\x00\x00\x00\x00\x00\x01\x03"


def _find_document(body):
    """Return (offset, extension, format_name) for a captured MintPRINT job."""
    signatures = (
        (b"%!PS-Adobe-", ".ps", "PostScript"),
        (b"%PDF-", ".pdf", "PDF"),
        (b"\xff\xd8\xff", ".jpg", "JPEG"),
        (b"RaS2", ".pwg", "PWG Raster"),
    )
    best = None
    for signature, extension, name in signatures:
        pos = body.find(signature)
        if pos >= 0 and (best is None or pos < best[0]):
            best = (pos, extension, name)
    if best is not None:
        return best
    return 0, ".bin", "unknown/raw IPP body"


class MintPrintHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MintPRINTMockIPP/1.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))
        sys.stdout.flush()

    def do_POST(self):
        started = time.perf_counter()
        length_text = self.headers.get("Content-Length")
        try:
            content_length = int(length_text)
        except (TypeError, ValueError):
            self.send_error(411, "Content-Length required")
            return

        body = self.rfile.read(content_length)
        receive_seconds = time.perf_counter() - started
        if len(body) != content_length:
            self.send_error(400, "Short request body")
            return

        doc_offset, extension, format_name = _find_document(body)
        document = body[doc_offset:]
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.server.job_number += 1
        filename = "mintprint-%s-%03d%s" % (
            stamp, self.server.job_number, extension
        )
        output_path = os.path.join(self.server.output_dir, filename)
        with open(output_path, "wb") as output:
            output.write(document)

        mib = 1024.0 * 1024.0
        rate = ((len(body) / mib) / receive_seconds) if receive_seconds > 0 else 0.0
        print(
            "Captured job %d: %s, HTTP body=%d bytes, document=%d bytes, "
            "receive=%.3fs (%.2f MiB/s)"
            % (
                self.server.job_number,
                format_name,
                len(body),
                len(document),
                receive_seconds,
                rate,
            )
        )
        print("Saved: %s" % output_path)
        sys.stdout.flush()

        self.send_response(200, "OK")
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(IPP_OK)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(IPP_OK)
        self.wfile.flush()
        self.close_connection = True

        if self.server.once:
            self.server.stop_after_request = True


class MintPrintHTTPServer(HTTPServer):
    def __init__(self, address, handler, output_dir, once=False):
        HTTPServer.__init__(self, address, handler)
        self.output_dir = output_dir
        self.once = once
        self.stop_after_request = False
        self.job_number = 0


def main():
    parser = argparse.ArgumentParser(
        description="Accept MintPRINT IPP jobs locally and save the document."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=8631, help="listen port")
    parser.add_argument(
        "--output-dir", default="ipp-captures", help="directory for captured jobs"
    )
    parser.add_argument(
        "--once", action="store_true", help="exit after one captured job"
    )
    args = parser.parse_args()

    if not os.path.isdir(args.output_dir):
        os.makedirs(args.output_dir)

    server = MintPrintHTTPServer(
        (args.bind, args.port), MintPrintHandler, args.output_dir, args.once
    )
    print("MintPRINT mock IPP sink listening on %s:%d" % (args.bind, args.port))
    print("MintPRINT path can remain /ipp/print (all POST paths are accepted).")
    print("Captured documents will be written to %s" % args.output_dir)
    sys.stdout.flush()

    try:
        while not server.stop_after_request:
            server.handle_request()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
