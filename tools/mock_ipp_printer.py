#!/usr/bin/env python3
"""Local IPP test printer for MintPRINT benchmarking.

The helper does two jobs:

* Get-Printer-Attributes: returns a small deterministic capability set so
  MintPrint Settings can populate its Engine/Scaling/DPI controls without a
  physical printer.
* Print-Job: saves the submitted document and immediately returns IPP
  successful-ok, removing physical-printer processing time from benchmarks.

Example:
    python tools/mock_ipp_printer.py --port 8631 --output-dir ipp-captures

Point MintPRINT at the PC's LAN address, port 8631, path /ipp/print, then
press Query once in MintPrint Settings before selecting the test options.
"""

from __future__ import print_function

import argparse
import os
import struct
import sys
import time

try:
    from http.server import BaseHTTPRequestHandler, HTTPServer
except ImportError:  # Python 2 fallback, though Python 3 is recommended.
    from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer


IPP_PRINT_JOB = 0x0002
IPP_GET_PRINTER_ATTRIBUTES = 0x000B


def _as_bytes(value):
    if isinstance(value, bytes):
        return value
    return value.encode("ascii")


def _ipp_header(request, status=0):
    version = request[:2] if len(request) >= 2 else b"\x01\x01"
    request_id = request[4:8] if len(request) >= 8 else b"\x00\x00\x00\x01"
    return version + struct.pack(">H", status) + request_id


def _ipp_attr(tag, name, value):
    name_bytes = _as_bytes(name)
    value_bytes = _as_bytes(value)
    return (
        struct.pack(">BH", tag, len(name_bytes))
        + name_bytes
        + struct.pack(">H", len(value_bytes))
        + value_bytes
    )


def _ipp_more(tag, value):
    """Additional value for the immediately preceding IPP attribute."""
    value_bytes = _as_bytes(value)
    return struct.pack(">BH", tag, 0) + struct.pack(">H", len(value_bytes)) + value_bytes


def _ipp_resolution(name, dpi, first=True):
    value = struct.pack(">IIB", dpi, dpi, 3)  # units=3 => dots per inch
    if first:
        return _ipp_attr(0x32, name, value)
    return _ipp_more(0x32, value)


def _get_printer_attributes_response(request):
    """Return deterministic capabilities useful for MintPRINT development."""
    out = bytearray(_ipp_header(request))

    # Operation attributes.
    out += b"\x01"
    out += _ipp_attr(0x47, "attributes-charset", "utf-8")
    out += _ipp_attr(0x48, "attributes-natural-language", "en")

    # Printer attributes. Keep this deliberately small and predictable.
    out += b"\x04"
    out += _ipp_attr(0x42, "printer-name", "MintPRINT Mock IPP")
    out += _ipp_attr(0x41, "printer-make-and-model", "MintPRINT Mock Test Printer")

    out += _ipp_attr(0x49, "document-format-supported", "application/postscript")
    out += _ipp_more(0x49, "image/jpeg")
    out += _ipp_more(0x49, "image/pwg-raster")
    out += _ipp_more(0x49, "application/pdf")

    out += _ipp_attr(0x44, "print-scaling-supported", "auto")
    out += _ipp_more(0x44, "auto-fit")
    out += _ipp_more(0x44, "fit")
    out += _ipp_more(0x44, "fill")
    out += _ipp_more(0x44, "none")

    out += _ipp_attr(0x44, "media-supported", "iso_a4_210x297mm")

    out += _ipp_attr(0x44, "print-color-mode-supported", "color")
    out += _ipp_more(0x44, "monochrome")

    out += _ipp_attr(0x44, "sides-supported", "one-sided")

    out += _ipp_resolution("printer-resolution-supported", 300, True)
    out += _ipp_resolution("printer-resolution-supported", 600, False)

    # Operations-supported is useful diagnostic metadata and makes this look
    # like a normal tiny IPP printer rather than a special-case Query reply.
    out += _ipp_attr(0x23, "operations-supported", struct.pack(">I", IPP_PRINT_JOB))
    out += _ipp_more(0x23, struct.pack(">I", IPP_GET_PRINTER_ATTRIBUTES))

    out += b"\x03"
    return bytes(out)


def _successful_ok_response(request):
    return _ipp_header(request) + b"\x03"


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
    server_version = "MintPRINTMockIPP/1.1"

    def log_message(self, fmt, *args):
        sys.stdout.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))
        sys.stdout.flush()

    def _send_ipp(self, response):
        self.send_response(200, "OK")
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(response)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(response)
        self.wfile.flush()
        self.close_connection = True

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
        if len(body) < 8:
            self.send_error(400, "IPP header too short")
            return

        operation = struct.unpack(">H", body[2:4])[0]

        if operation == IPP_GET_PRINTER_ATTRIBUTES:
            response = _get_printer_attributes_response(body)
            print(
                "Query: advertised PostScript/JPEG/PWG/PDF, A4, 300/600 dpi, "
                "scaling auto/auto-fit/fit/fill/none"
            )
            sys.stdout.flush()
            self._send_ipp(response)
            return

        if operation == IPP_PRINT_JOB:
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
            rate = (
                ((len(body) / mib) / receive_seconds)
                if receive_seconds > 0
                else 0.0
            )
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

            self._send_ipp(_successful_ok_response(body))

            if self.server.once:
                self.server.stop_after_request = True
            return

        print("IPP operation 0x%04x: returning successful-ok" % operation)
        sys.stdout.flush()
        self._send_ipp(_successful_ok_response(body))


class MintPrintHTTPServer(HTTPServer):
    def __init__(self, address, handler, output_dir, once=False):
        HTTPServer.__init__(self, address, handler)
        self.output_dir = output_dir
        self.once = once
        self.stop_after_request = False
        self.job_number = 0


def main():
    parser = argparse.ArgumentParser(
        description="Provide deterministic IPP capabilities and capture MintPRINT jobs."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", type=int, default=8631, help="listen port")
    parser.add_argument(
        "--output-dir", default="ipp-captures", help="directory for captured jobs"
    )
    parser.add_argument(
        "--once", action="store_true", help="exit after one captured Print-Job"
    )
    args = parser.parse_args()

    if not os.path.isdir(args.output_dir):
        os.makedirs(args.output_dir)

    server = MintPrintHTTPServer(
        (args.bind, args.port), MintPrintHandler, args.output_dir, args.once
    )
    print("MintPRINT mock IPP printer listening on %s:%d" % (args.bind, args.port))
    print("MintPRINT path can remain /ipp/print (all POST paths are accepted).")
    print("Press Query in MintPrint Settings to load the mock capabilities.")
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
