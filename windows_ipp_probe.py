#!/usr/bin/env python3
"""
MintPRINT IPP capability probe.

A dependency-free diagnostic utility for checking an IPP/AirPrint printer
from Windows, macOS or Linux. It queries the printer for the capabilities
that matter to MintPRINT and can optionally validate MintPRINT document
formats or submit a real test file.

Python 3.8+; standard library only.
"""

import argparse
import getpass
import http.client
import json
import os
import ssl
import sys
import urllib.parse
from collections import defaultdict

VERSION = "2.3"

# IPP operations
OP_PRINT_JOB = 0x0002
OP_VALIDATE_JOB = 0x0004
OP_CREATE_JOB = 0x0005
OP_SEND_DOCUMENT = 0x0006
OP_GET_PRINTER_ATTRIBUTES = 0x000B

# Delimiter tags
GROUP_TAGS = {
    0x01: "operation-attributes",
    0x02: "job-attributes",
    0x04: "printer-attributes",
    0x05: "unsupported-attributes",
    0x06: "subscription-attributes",
    0x07: "event-notification-attributes",
    0x09: "resource-attributes",
    0x0A: "document-attributes",
    0x0F: "system-attributes",
}
TAG_END_OF_ATTRIBUTES = 0x03

# Value tags
TAG_INTEGER = 0x21
TAG_BOOLEAN = 0x22
TAG_ENUM = 0x23
TAG_OCTET_STRING = 0x30
TAG_DATETIME = 0x31
TAG_RESOLUTION = 0x32
TAG_RANGE_OF_INTEGER = 0x33
TAG_BEGIN_COLLECTION = 0x34
TAG_TEXT_WITH_LANGUAGE = 0x35
TAG_NAME_WITH_LANGUAGE = 0x36
TAG_END_COLLECTION = 0x37
TAG_TEXT = 0x41
TAG_NAME = 0x42
TAG_KEYWORD = 0x44
TAG_URI = 0x45
TAG_URI_SCHEME = 0x46
TAG_CHARSET = 0x47
TAG_NATURAL_LANGUAGE = 0x48
TAG_MIME_MEDIA_TYPE = 0x49
TAG_MEMBER_ATTR_NAME = 0x4A

STRING_TAGS = {
    TAG_TEXT, TAG_NAME, TAG_KEYWORD, TAG_URI, TAG_URI_SCHEME,
    TAG_CHARSET, TAG_NATURAL_LANGUAGE, TAG_MIME_MEDIA_TYPE,
    TAG_MEMBER_ATTR_NAME,
}

STATUS_NAMES = {
    0x0000: "successful-ok",
    0x0001: "successful-ok-ignored-or-substituted-attributes",
    0x0002: "successful-ok-conflicting-attributes",
    0x0400: "client-error-bad-request",
    0x0401: "client-error-forbidden",
    0x0402: "client-error-not-authenticated",
    0x0403: "client-error-not-authorized",
    0x0404: "client-error-not-possible",
    0x0405: "client-error-timeout",
    0x0406: "client-error-not-found",
    0x0407: "client-error-gone",
    0x0408: "client-error-request-entity-too-large",
    0x0409: "client-error-request-value-too-long",
    0x040A: "client-error-document-format-not-supported",
    0x040B: "client-error-attributes-or-values-not-supported",
    0x040C: "client-error-uri-scheme-not-supported",
    0x040D: "client-error-charset-not-supported",
    0x040E: "client-error-conflicting-attributes",
    0x040F: "client-error-compression-not-supported",
    0x0410: "client-error-compression-error",
    0x0411: "client-error-document-format-error",
    0x0412: "client-error-document-access-error",
    0x0413: "client-error-attributes-not-settable",
    0x0500: "server-error-internal-error",
    0x0501: "server-error-operation-not-supported",
    0x0502: "server-error-service-unavailable",
    0x0503: "server-error-version-not-supported",
    0x0504: "server-error-device-error",
    0x0505: "server-error-temporary-error",
    0x0506: "server-error-not-accepting-jobs",
    0x0507: "server-error-busy",
    0x0508: "server-error-job-canceled",
    0x0509: "server-error-multiple-document-jobs-not-supported",
}

OPERATION_NAMES = {
    0x0002: "Print-Job",
    0x0003: "Print-URI",
    0x0004: "Validate-Job",
    0x0005: "Create-Job",
    0x0006: "Send-Document",
    0x0007: "Send-URI",
    0x0008: "Cancel-Job",
    0x0009: "Get-Job-Attributes",
    0x000A: "Get-Jobs",
    0x000B: "Get-Printer-Attributes",
    0x0010: "Pause-Printer",
    0x0011: "Resume-Printer",
    0x0012: "Purge-Jobs",
}

PRINT_QUALITY_NAMES = {3: "draft", 4: "normal", 5: "high"}
ORIENTATION_NAMES = {
    3: "portrait",
    4: "landscape",
    5: "reverse-landscape",
    6: "reverse-portrait",
}
PRINTER_STATE_NAMES = {3: "idle", 4: "processing", 5: "stopped"}

# Bounded, MintPRINT-focused request instead of blindly asking for "all".
REQUESTED_ATTRIBUTES = [
    "printer-name",
    "printer-info",
    "printer-location",
    "printer-make-and-model",
    "printer-device-id",
    "printer-icons",
    "printer-uri-supported",
    "uri-authentication-supported",
    "uri-security-supported",
    "ipp-versions-supported",
    "ipp-features-supported",
    "operations-supported",
    "printer-state",
    "printer-state-reasons",
    "printer-is-accepting-jobs",
    "queued-job-count",
    "document-format-default",
    "document-format-preferred",
    "document-format-supported",
    "jpeg-k-octets-supported",
    "jpeg-x-dimension-supported",
    "jpeg-y-dimension-supported",
    # RFC 3805 / PWG5100.13 Printer MIB ink/toner status - kept in sync
    # with MintPrintSettings.c's mp_requested_attrs[], which now also
    # requests these for the GUI's ink-status panel (1.2.3).
    "marker-names",
    "marker-colors",
    "marker-types",
    "marker-levels",
    "marker-low-levels",
    "marker-high-levels",
    "compression-supported",
    "charset-configured",
    "charset-supported",
    "natural-language-configured",
    "generated-natural-language-supported",
    "job-creation-attributes-supported",
    "multiple-document-jobs-supported",
    "multiple-document-handling-supported",
    "copies-default",
    "copies-supported",
    "media-default",
    "media-ready",
    "media-supported",
    "media-source-default",
    "media-source-supported",
    "media-type-default",
    "media-type-supported",
    "media-col-default",
    "media-col-ready",
    "media-col-supported",
    "media-left-margin-supported",
    "media-right-margin-supported",
    "media-top-margin-supported",
    "media-bottom-margin-supported",
    "printer-resolution-default",
    "printer-resolution-supported",
    "pwg-raster-document-resolution-supported",
    "pwg-raster-document-type-supported",
    "pwg-raster-document-sheet-back",
    "urf-supported",
    "pdf-versions-supported",
    "color-supported",
    "print-color-mode-default",
    "print-color-mode-supported",
    "print-quality-default",
    "print-quality-supported",
    "print-scaling-default",
    "print-scaling-supported",
    "orientation-requested-default",
    "orientation-requested-supported",
    "sides-default",
    "sides-supported",
    "finishings-supported",
    "output-bin-default",
    "output-bin-supported",
]


class IPPError(Exception):
    pass


def be16(value):
    return int(value).to_bytes(2, "big", signed=False)


def be32(value):
    return int(value).to_bytes(4, "big", signed=False)


def attr(tag, name, value):
    name_b = name.encode("utf-8")
    if isinstance(value, str):
        value_b = value.encode("utf-8")
    else:
        value_b = bytes(value)
    return bytes([tag]) + be16(len(name_b)) + name_b + be16(len(value_b)) + value_b


def attr_repeat(tag, value):
    if isinstance(value, str):
        value_b = value.encode("utf-8")
    else:
        value_b = bytes(value)
    return bytes([tag]) + b"\x00\x00" + be16(len(value_b)) + value_b


def multi_attr(tag, name, values):
    values = list(values)
    if not values:
        return b""
    out = bytearray(attr(tag, name, values[0]))
    for value in values[1:]:
        out += attr_repeat(tag, value)
    return bytes(out)


def normalize_target(raw):
    if "://" not in raw:
        raw = "http://" + raw

    parsed = urllib.parse.urlsplit(raw)
    scheme = parsed.scheme.lower()
    if scheme not in ("http", "https", "ipp", "ipps"):
        raise ValueError("URL scheme must be http, https, ipp, or ipps")
    if not parsed.hostname:
        raise ValueError("URL does not contain a hostname")

    if scheme in ("ipp", "http"):
        transport_scheme = "http"
        printer_scheme = "ipp"
        default_port = 631
    elif scheme == "ipps":
        transport_scheme = "https"
        printer_scheme = "ipps"
        default_port = 631
    else:  # explicit https://
        transport_scheme = "https"
        printer_scheme = "ipps"
        default_port = 443

    port = parsed.port or default_port
    path = parsed.path or "/ipp/print"
    if not path.startswith("/"):
        path = "/" + path

    host = parsed.hostname
    authority_host = "[%s]" % host if ":" in host and not host.startswith("[") else host
    transport_netloc = "%s:%d" % (authority_host, port)

    # ipp/ipps default port is 631. Keep non-default ports explicit.
    if port == 631:
        printer_netloc = authority_host
    else:
        printer_netloc = "%s:%d" % (authority_host, port)

    query = ("?" + parsed.query) if parsed.query else ""
    transport_url = "%s://%s%s%s" % (
        transport_scheme, transport_netloc, path, query
    )
    printer_uri = "%s://%s%s" % (printer_scheme, printer_netloc, path)

    return {
        "input": raw,
        "host": host,
        "port": port,
        "path": path + query,
        "transport_scheme": transport_scheme,
        "transport_url": transport_url,
        "printer_uri": printer_uri,
    }


def base_request(operation_id, request_id, printer_uri):
    body = bytearray()
    body += b"\x02\x00"                 # IPP/2.0; widely accepted by IPP Everywhere printers.
    body += be16(operation_id)
    body += be32(request_id)
    body += b"\x01"                     # operation-attributes-tag
    body += attr(TAG_CHARSET, "attributes-charset", "utf-8")
    body += attr(TAG_NATURAL_LANGUAGE, "attributes-natural-language", "en")
    body += attr(TAG_URI, "printer-uri", printer_uri)
    return body


def build_get_printer_attributes(printer_uri, request_id=1, request_all=False):
    body = base_request(OP_GET_PRINTER_ATTRIBUTES, request_id, printer_uri)
    if request_all:
        body += attr(TAG_KEYWORD, "requested-attributes", "all")
    else:
        body += multi_attr(TAG_KEYWORD, "requested-attributes", REQUESTED_ATTRIBUTES)
    body += bytes([TAG_END_OF_ATTRIBUTES])
    return bytes(body)


def build_validate_job(printer_uri, mime, request_id):
    body = base_request(OP_VALIDATE_JOB, request_id, printer_uri)
    body += attr(TAG_NAME, "requesting-user-name", safe_username())
    body += attr(TAG_MIME_MEDIA_TYPE, "document-format", mime)
    body += bytes([TAG_END_OF_ATTRIBUTES])
    return bytes(body)


def build_print_job(printer_uri, mime, job_name, request_id, sides=None):
    body = base_request(OP_PRINT_JOB, request_id, printer_uri)
    body += attr(TAG_NAME, "requesting-user-name", safe_username())
    body += attr(TAG_NAME, "job-name", job_name)
    body += attr(TAG_MIME_MEDIA_TYPE, "document-format", mime)
    if sides:
        # job-attributes-tag (0x02), matching MintPRINT's own driver
        # (driver/ipp_client.c) so a --print-file test reproduces the same
        # request a real duplex job sends, including the "sides" attribute
        # a duplex URF/PWG stream depends on the printer actually honouring.
        body += bytes([0x02])
        body += attr(TAG_KEYWORD, "sides", sides)
    body += bytes([TAG_END_OF_ATTRIBUTES])
    return bytes(body)


def safe_username():
    try:
        return getpass.getuser() or "MintPRINT"
    except Exception:
        return "MintPRINT"


def send_ipp(target, body, payload=b"", timeout=15.0, insecure=False):
    if target["transport_scheme"] == "https":
        context = ssl._create_unverified_context() if insecure else ssl.create_default_context()
        conn = http.client.HTTPSConnection(
            target["host"], target["port"], timeout=timeout, context=context
        )
    else:
        conn = http.client.HTTPConnection(
            target["host"], target["port"], timeout=timeout
        )

    data = body + payload
    headers = {
        "Content-Type": "application/ipp",
        "Content-Length": str(len(data)),
        "User-Agent": "MintPRINT-IPP-Probe/%s" % VERSION,
        "Connection": "close",
    }

    try:
        conn.request("POST", target["path"], body=data, headers=headers)
        response = conn.getresponse()
        response_body = response.read()
        return response.status, response.reason, response_body
    finally:
        conn.close()


def signed32(raw):
    return int.from_bytes(raw, "big", signed=True)


def decode_text_with_language(value):
    try:
        if len(value) < 4:
            raise ValueError
        lang_len = int.from_bytes(value[0:2], "big")
        p = 2
        lang = value[p:p + lang_len].decode("utf-8", "replace")
        p += lang_len
        text_len = int.from_bytes(value[p:p + 2], "big")
        p += 2
        text = value[p:p + text_len].decode("utf-8", "replace")
        return {"language": lang, "text": text}
    except Exception:
        return value.hex()


def decode_value(tag, value):
    # Out-of-band values.
    if tag == 0x10:
        return "<unsupported>"
    if tag == 0x12:
        return "<unknown>"
    if tag == 0x13:
        return "<no-value>"

    if tag == TAG_INTEGER and len(value) == 4:
        return signed32(value)
    if tag == TAG_BOOLEAN and len(value) == 1:
        return bool(value[0])
    if tag == TAG_ENUM and len(value) == 4:
        return signed32(value)
    if tag == TAG_RANGE_OF_INTEGER and len(value) == 8:
        return {"lower": signed32(value[:4]), "upper": signed32(value[4:8])}
    if tag == TAG_RESOLUTION and len(value) == 9:
        xres = signed32(value[:4])
        yres = signed32(value[4:8])
        units = {3: "dpi", 4: "dpcm"}.get(value[8], "unit-%d" % value[8])
        return {"x": xres, "y": yres, "units": units}
    if tag == TAG_DATETIME and len(value) >= 11:
        year = int.from_bytes(value[0:2], "big")
        sign = chr(value[8]) if value[8] in (ord("+"), ord("-")) else "?"
        return "%04d-%02d-%02dT%02d:%02d:%02d.%d%s%02d:%02d" % (
            year, value[2], value[3], value[4], value[5], value[6],
            value[7], sign, value[9], value[10]
        )
    if tag in (TAG_TEXT_WITH_LANGUAGE, TAG_NAME_WITH_LANGUAGE):
        return decode_text_with_language(value)
    if tag in STRING_TAGS:
        return value.decode("utf-8", "replace")
    if tag == TAG_OCTET_STRING:
        # IEEE-1284 device IDs are often exposed as octets but are readable text.
        try:
            text = value.decode("utf-8")
            if all((ch.isprintable() or ch in "\r\n\t") for ch in text):
                return text
        except Exception:
            pass
        return "0x" + value.hex()

    # Unknown value type: preserve the bytes rather than crashing the probe.
    return {"tag": "0x%02x" % tag, "hex": value.hex()}


def read_field(data, pos, size):
    end = pos + size
    if end > len(data):
        raise IPPError("Truncated IPP response")
    return data[pos:end], end


def parse_collection(data, pos):
    result = defaultdict(list)
    current_member = None

    while pos < len(data):
        tag = data[pos]
        pos += 1

        name_len_b, pos = read_field(data, pos, 2)
        name_len = int.from_bytes(name_len_b, "big")
        name_b, pos = read_field(data, pos, name_len)

        value_len_b, pos = read_field(data, pos, 2)
        value_len = int.from_bytes(value_len_b, "big")
        value_b, pos = read_field(data, pos, value_len)

        if tag == TAG_END_COLLECTION:
            return dict(result), pos

        name = name_b.decode("utf-8", "replace") if name_b else ""

        if tag == TAG_MEMBER_ATTR_NAME:
            current_member = value_b.decode("utf-8", "replace")
            continue

        member_name = name or current_member or "<unnamed>"

        if tag == TAG_BEGIN_COLLECTION:
            nested, pos = parse_collection(data, pos)
            result[member_name].append(nested)
        else:
            result[member_name].append(decode_value(tag, value_b))

    raise IPPError("Unterminated IPP collection")


def parse_ipp_response(data):
    if len(data) < 8:
        raise IPPError("IPP response is shorter than the 8-byte header")

    version = "%d.%d" % (data[0], data[1])
    status = int.from_bytes(data[2:4], "big")
    request_id = int.from_bytes(data[4:8], "big")

    groups = defaultdict(lambda: defaultdict(list))
    current_group = "unknown-group"
    previous_name = None
    pos = 8

    while pos < len(data):
        tag = data[pos]
        pos += 1

        if tag == TAG_END_OF_ATTRIBUTES:
            break

        if tag in GROUP_TAGS:
            current_group = GROUP_TAGS[tag]
            previous_name = None
            continue

        name_len_b, pos = read_field(data, pos, 2)
        name_len = int.from_bytes(name_len_b, "big")
        name_b, pos = read_field(data, pos, name_len)

        value_len_b, pos = read_field(data, pos, 2)
        value_len = int.from_bytes(value_len_b, "big")
        value_b, pos = read_field(data, pos, value_len)

        if name_len:
            name = name_b.decode("utf-8", "replace")
            previous_name = name
        else:
            name = previous_name or "<unnamed>"

        if tag == TAG_BEGIN_COLLECTION:
            decoded, pos = parse_collection(data, pos)
        else:
            decoded = decode_value(tag, value_b)

        groups[current_group][name].append(decoded)

    return {
        "version": version,
        "status": status,
        "status_name": STATUS_NAMES.get(status, "unknown-status"),
        "request_id": request_id,
        "groups": {g: dict(attrs) for g, attrs in groups.items()},
    }


def all_values(parsed, name):
    values = []
    # Prefer printer attrs but search all groups so unsupported attrs can still be shown.
    for group_name in (
        "printer-attributes",
        "operation-attributes",
        "job-attributes",
        "unsupported-attributes",
        "unknown-group",
    ):
        values.extend(parsed["groups"].get(group_name, {}).get(name, []))
    return values


def first_value(parsed, name, default=None):
    values = all_values(parsed, name)
    return values[0] if values else default


def flat_strings(values):
    out = []
    for value in values:
        if isinstance(value, dict) and "text" in value:
            out.append(str(value["text"]))
        elif isinstance(value, (str, int, bool)):
            out.append(str(value))
    return out


def format_scalar(value):
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, dict):
        if set(value.keys()) == {"lower", "upper"}:
            return "%s..%s" % (value["lower"], value["upper"])
        if set(value.keys()) == {"x", "y", "units"}:
            return "%sx%s %s" % (value["x"], value["y"], value["units"])
        if "language" in value and "text" in value:
            return str(value["text"])
        return json.dumps(value, sort_keys=True, ensure_ascii=False)
    return str(value)


def format_values(values, enum_map=None):
    rendered = []
    for value in values:
        if enum_map and isinstance(value, int):
            rendered.append("%s (%s)" % (enum_map.get(value, "enum"), value))
        else:
            rendered.append(format_scalar(value))
    return ", ".join(rendered)


def add_list(lines, label, values, enum_map=None, max_items=40):
    if not values:
        lines.append("  %-27s %s" % (label + ":", "<not reported>"))
        return
    shown = values[:max_items]
    text = format_values(shown, enum_map=enum_map)
    if len(values) > max_items:
        text += " ... (+%d more)" % (len(values) - max_items)
    lines.append("  %-27s %s" % (label + ":", text))


def pretty_collection(value, indent="    "):
    """Render nested IPP collection values without assuming a fixed shape."""
    if isinstance(value, dict):
        lines = []
        for key in sorted(value):
            item = value[key]

            if isinstance(item, list):
                if len(item) == 1 and not isinstance(item[0], (dict, list)):
                    lines.append("%s%s: %s" % (indent, key, format_scalar(item[0])))
                    continue

                lines.append("%s%s:" % (indent, key))
                for child in item:
                    if isinstance(child, (dict, list)):
                        lines.extend(pretty_collection(child, indent + "  "))
                    else:
                        lines.append(indent + "  - " + format_scalar(child))
                continue

            if isinstance(item, (dict, list)):
                lines.append("%s%s:" % (indent, key))
                lines.extend(pretty_collection(item, indent + "  "))
            else:
                lines.append("%s%s: %s" % (indent, key, format_scalar(item)))

        return lines

    if isinstance(value, list):
        lines = []
        for item in value:
            if isinstance(item, (dict, list)):
                lines.extend(pretty_collection(item, indent))
            else:
                lines.append(indent + "- " + format_scalar(item))
        return lines

    return [indent + format_scalar(value)]


def report(parsed, target, http_status, http_reason, response_bytes, dump_all=False):
    lines = []
    lines.append("MintPRINT IPP Capability Probe v%s" % VERSION)
    lines.append("=" * 66)
    lines.append("")
    lines.append("Connection")
    lines.append("  Transport URL:              %s" % target["transport_url"])
    lines.append("  Printer URI:                %s" % target["printer_uri"])
    lines.append("  HTTP status:                %s %s" % (http_status, http_reason))
    lines.append("  IPP version:                %s" % parsed["version"])
    lines.append("  IPP status:                 %s (0x%04x)" % (
        parsed["status_name"], parsed["status"]
    ))
    lines.append("  IPP response bytes:         %d" % response_bytes)
    lines.append("")

    lines.append("Printer")
    add_list(lines, "Name", all_values(parsed, "printer-name"), max_items=5)
    add_list(lines, "Make / model", all_values(parsed, "printer-make-and-model"), max_items=5)
    add_list(lines, "Info", all_values(parsed, "printer-info"), max_items=5)
    add_list(lines, "Location", all_values(parsed, "printer-location"), max_items=5)
    add_list(lines, "Printer URI(s)", all_values(parsed, "printer-uri-supported"), max_items=10)
    add_list(lines, "IPP versions", all_values(parsed, "ipp-versions-supported"), max_items=20)
    add_list(lines, "URI security", all_values(parsed, "uri-security-supported"), max_items=10)
    add_list(lines, "URI authentication", all_values(parsed, "uri-authentication-supported"), max_items=10)
    add_list(lines, "State", all_values(parsed, "printer-state"), PRINTER_STATE_NAMES, 5)
    add_list(lines, "State reasons", all_values(parsed, "printer-state-reasons"), max_items=20)
    add_list(lines, "Accepting jobs", all_values(parsed, "printer-is-accepting-jobs"), max_items=5)
    add_list(lines, "Queued jobs", all_values(parsed, "queued-job-count"), max_items=5)
    lines.append("")

    lines.append("Ink/toner status (RFC 3805 / PWG5100.13)")
    add_list(lines, "Marker names", all_values(parsed, "marker-names"), max_items=10)
    add_list(lines, "Marker colors", all_values(parsed, "marker-colors"), max_items=10)
    add_list(lines, "Marker types", all_values(parsed, "marker-types"), max_items=10)
    add_list(lines, "Marker levels", all_values(parsed, "marker-levels"), max_items=10)
    add_list(lines, "Marker low levels", all_values(parsed, "marker-low-levels"), max_items=10)
    add_list(lines, "Marker high levels", all_values(parsed, "marker-high-levels"), max_items=10)
    lines.append("")

    formats = [s.lower() for s in flat_strings(all_values(parsed, "document-format-supported"))]
    jpeg_constraint_names = (
        "jpeg-k-octets-supported",
        "jpeg-x-dimension-supported",
        "jpeg-y-dimension-supported",
    )
    jpeg_constraints = any(all_values(parsed, name) for name in jpeg_constraint_names)
    lines.append("Document formats")
    add_list(lines, "Default", all_values(parsed, "document-format-default"), max_items=5)
    add_list(lines, "Preferred", all_values(parsed, "document-format-preferred"), max_items=5)
    add_list(lines, "Supported", all_values(parsed, "document-format-supported"), max_items=60)
    lines.append("")
    lines.append("MintPRINT engine compatibility")
    jpeg_status = "NO"
    if "image/jpeg" in formats:
        jpeg_status = "YES" if jpeg_constraints else "NOMINAL (no JPEG constraints reported)"
    lines.append("  %-27s %s" % ("JPEG (image/jpeg):", jpeg_status))
    lines.append("  %-27s %s" % ("PostScript:", "YES" if "application/postscript" in formats else "NO"))
    lines.append("  %-27s %s" % ("PWG Raster:", "YES" if "image/pwg-raster" in formats else "NO"))
    lines.append("  %-27s %s" % ("PDF (application/pdf):", "YES" if "application/pdf" in formats else "NO"))
    lines.append("  %-27s %s" % ("Apple Raster (image/urf):", "YES" if "image/urf" in formats else "NO"))
    add_list(lines, "JPEG size limit", all_values(parsed, "jpeg-k-octets-supported"), max_items=10)
    add_list(lines, "JPEG width limit", all_values(parsed, "jpeg-x-dimension-supported"), max_items=10)
    add_list(lines, "JPEG height limit", all_values(parsed, "jpeg-y-dimension-supported"), max_items=10)
    add_list(lines, "PWG raster type(s)", all_values(parsed, "pwg-raster-document-type-supported"), max_items=30)
    add_list(lines, "PWG resolution(s)", all_values(parsed, "pwg-raster-document-resolution-supported"), max_items=30)
    add_list(lines, "PWG sheet-back", all_values(parsed, "pwg-raster-document-sheet-back"), max_items=10)
    add_list(lines, "URF", all_values(parsed, "urf-supported"), max_items=30)
    add_list(lines, "PDF versions", all_values(parsed, "pdf-versions-supported"), max_items=20)
    lines.append("")

    lines.append("Page / media geometry")
    add_list(lines, "Media default", all_values(parsed, "media-default"), max_items=5)
    add_list(lines, "Media ready", all_values(parsed, "media-ready"), max_items=30)
    add_list(lines, "Media supported", all_values(parsed, "media-supported"), max_items=40)
    add_list(lines, "Source default", all_values(parsed, "media-source-default"), max_items=5)
    add_list(lines, "Sources supported", all_values(parsed, "media-source-supported"), max_items=30)
    add_list(lines, "Media type default", all_values(parsed, "media-type-default"), max_items=5)
    add_list(lines, "Media types", all_values(parsed, "media-type-supported"), max_items=30)
    add_list(lines, "Resolution default", all_values(parsed, "printer-resolution-default"), max_items=5)
    add_list(lines, "Resolutions supported", all_values(parsed, "printer-resolution-supported"), max_items=30)
    add_list(lines, "Left margins", all_values(parsed, "media-left-margin-supported"), max_items=20)
    add_list(lines, "Right margins", all_values(parsed, "media-right-margin-supported"), max_items=20)
    add_list(lines, "Top margins", all_values(parsed, "media-top-margin-supported"), max_items=20)
    add_list(lines, "Bottom margins", all_values(parsed, "media-bottom-margin-supported"), max_items=20)

    media_col_default = all_values(parsed, "media-col-default")
    if media_col_default:
        lines.append("  Media-col default:")
        for item in media_col_default:
            lines.extend(pretty_collection(item, "    "))
    else:
        lines.append("  %-27s %s" % ("Media-col default:", "<not reported>"))
    lines.append("")

    lines.append("Job options")
    add_list(lines, "Colour supported", all_values(parsed, "color-supported"), max_items=5)
    add_list(lines, "Colour mode default", all_values(parsed, "print-color-mode-default"), max_items=5)
    add_list(lines, "Colour modes", all_values(parsed, "print-color-mode-supported"), max_items=20)
    add_list(lines, "Quality default", all_values(parsed, "print-quality-default"), PRINT_QUALITY_NAMES, 5)
    add_list(lines, "Quality supported", all_values(parsed, "print-quality-supported"), PRINT_QUALITY_NAMES, 10)
    add_list(lines, "Scaling default", all_values(parsed, "print-scaling-default"), max_items=5)
    add_list(lines, "Scaling supported", all_values(parsed, "print-scaling-supported"), max_items=20)
    add_list(lines, "Orientation default", all_values(parsed, "orientation-requested-default"), ORIENTATION_NAMES, 5)
    add_list(lines, "Orientations", all_values(parsed, "orientation-requested-supported"), ORIENTATION_NAMES, 10)
    add_list(lines, "Sides default", all_values(parsed, "sides-default"), max_items=5)
    add_list(lines, "Sides supported", all_values(parsed, "sides-supported"), max_items=20)
    add_list(lines, "Multi-doc jobs", all_values(parsed, "multiple-document-jobs-supported"), max_items=5)
    add_list(lines, "Multi-doc handling", all_values(parsed, "multiple-document-handling-supported"), max_items=20)
    add_list(lines, "Copies supported", all_values(parsed, "copies-supported"), max_items=10)
    add_list(lines, "Output bins", all_values(parsed, "output-bin-supported"), max_items=20)
    lines.append("")

    operations = [v for v in all_values(parsed, "operations-supported") if isinstance(v, int)]
    lines.append("IPP operations")
    for op in (OP_PRINT_JOB, OP_VALIDATE_JOB, OP_CREATE_JOB,
               OP_SEND_DOCUMENT, OP_GET_PRINTER_ATTRIBUTES):
        lines.append("  %-27s %s" % (
            OPERATION_NAMES[op] + ":",
            "YES" if op in operations else ("UNKNOWN" if not operations else "NO")
        ))
    if operations:
        op_text = []
        for op in operations:
            op_text.append("%s (0x%04x)" % (OPERATION_NAMES.get(op, "op"), op))
        lines.append("  Advertised:                 " + ", ".join(op_text))
    else:
        lines.append("  Advertised:                 <not reported>")
    lines.append("")

    lines.append("MintPRINT diagnostic summary")
    warnings = []
    if parsed["status"] >= 0x0400:
        warnings.append("Printer rejected the capability request.")
    if http_status >= 400:
        warnings.append("HTTP transport returned an error.")
    if formats and not any(x in formats for x in (
        "image/jpeg", "application/postscript", "image/pwg-raster", "application/pdf",
        "image/urf"
    )):
        warnings.append("Printer advertises none of MintPRINT's JPEG/PostScript/PWG/PDF/URF engines.")
    if not formats:
        warnings.append("Printer did not report document-format-supported.")
    if "image/jpeg" in formats and not jpeg_constraints:
        warnings.append(
            "JPEG is advertised without JPEG size/dimension constraints; "
            "support is nominal and jobs may be silently discarded."
        )
    if operations and OP_PRINT_JOB not in operations:
        warnings.append("Printer does not advertise the IPP Print-Job operation.")
    duplex_sides = [value for value in all_values(parsed, "sides-supported")
                    if value in ("two-sided-long-edge", "two-sided-short-edge")]
    if duplex_sides and not any(x in formats for x in (
            "image/pwg-raster", "image/urf")):
        warnings.append(
            "Printer advertises duplex sides but neither multi-page PWG "
            "Raster nor Apple Raster (URF), as required by MintPRINT duplex."
        )
    if first_value(parsed, "printer-is-accepting-jobs") is False:
        warnings.append("Printer reports that it is not accepting jobs.")
    if first_value(parsed, "printer-state") == 5:
        warnings.append("Printer state is STOPPED; inspect printer-state-reasons.")

    geometry_attrs = (
        "media-default", "media-col-default", "printer-resolution-supported",
        "print-scaling-supported", "orientation-requested-supported"
    )
    if any(all_values(parsed, x) for x in geometry_attrs):
        lines.append("  Page-geometry data:         captured")
    else:
        lines.append("  Page-geometry data:         sparse / not reported")

    if warnings:
        for warning in warnings:
            lines.append("  WARNING: " + warning)
    else:
        lines.append("  No obvious IPP capability problem found.")
    lines.append("")

    unsupported = parsed["groups"].get("unsupported-attributes", {})
    if unsupported:
        lines.append("Unsupported requested attributes")
        for name in sorted(unsupported):
            lines.append("  %s: %s" % (name, format_values(unsupported[name])))
        lines.append("")

    if dump_all:
        lines.append("All parsed IPP attributes")
        lines.append("-" * 66)
        for group_name in sorted(parsed["groups"]):
            lines.append("[%s]" % group_name)
            for name in sorted(parsed["groups"][group_name]):
                values = parsed["groups"][group_name][name]
                if any(isinstance(v, dict) and not (
                    set(v.keys()) in ({"lower", "upper"}, {"x", "y", "units"})
                ) for v in values):
                    lines.append("  %s:" % name)
                    for v in values:
                        lines.extend(pretty_collection(v, "    "))
                else:
                    lines.append("  %s = %s" % (name, format_values(values)))
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def result_line(prefix, http_status, http_reason, parsed):
    return "%s: HTTP=%d %s, IPP=%s (0x%04x)" % (
        prefix, http_status, http_reason,
        parsed["status_name"], parsed["status"]
    )


def main():
    parser = argparse.ArgumentParser(
        description="MintPRINT IPP/AirPrint capability probe (no third-party modules required)"
    )
    parser.add_argument(
        "printer_url",
        help="e.g. http://192.168.0.3:631/ipp/print or ipp://192.168.0.3/ipp/print"
    )
    parser.add_argument(
        "--all", action="store_true",
        help="request all printer attributes and include a full parsed attribute dump"
    )
    parser.add_argument(
        "--output",
        help="also save the complete report to this text file"
    )
    parser.add_argument(
        "--timeout", type=float, default=15.0,
        help="network timeout in seconds (default: 15)"
    )
    parser.add_argument(
        "--insecure", action="store_true",
        help="for HTTPS/IPPS only: ignore certificate validation errors"
    )
    parser.add_argument(
        "--validate-mintprint", action="store_true",
        help="run non-printing Validate-Job checks for JPEG, PostScript, PWG Raster, PDF and URF"
    )
    parser.add_argument(
        "--print-file",
        help="optionally submit a real file after the capability query"
    )
    parser.add_argument(
        "--mime", default="image/jpeg",
        help="MIME type for --print-file (default: image/jpeg)"
    )
    parser.add_argument(
        "--sides",
        help="optional sides Job Template attribute for --print-file "
             "(e.g. two-sided-long-edge) - needed to reproduce a duplex "
             "job exactly as MintPRINT's own driver sends it"
    )
    args = parser.parse_args()

    try:
        target = normalize_target(args.printer_url)
    except Exception as exc:
        print("ERROR: invalid printer URL: %s" % exc, file=sys.stderr)
        return 2

    try:
        request = build_get_printer_attributes(
            target["printer_uri"], request_id=1, request_all=args.all
        )
        http_status, http_reason, response = send_ipp(
            target, request, timeout=args.timeout, insecure=args.insecure
        )
        parsed = parse_ipp_response(response)
    except ssl.SSLError as exc:
        print("ERROR: TLS certificate/handshake failed: %s" % exc, file=sys.stderr)
        print("If this is a trusted local printer with a self-signed certificate, retry with --insecure.", file=sys.stderr)
        return 3
    except (OSError, http.client.HTTPException, IPPError) as exc:
        print("ERROR: IPP query failed: %s" % exc, file=sys.stderr)
        return 3

    text = report(
        parsed, target, http_status, http_reason, len(response),
        dump_all=args.all
    )
    print(text, end="")

    extra = []

    if args.validate_mintprint:
        extra.append("")
        extra.append("Validate-Job checks (no page should be printed)")
        extra.append("-" * 66)
        for idx, mime in enumerate(
            ("image/jpeg", "application/postscript", "image/pwg-raster", "application/pdf",
             "image/urf"), start=10
        ):
            try:
                req = build_validate_job(target["printer_uri"], mime, idx)
                hs, hr, resp = send_ipp(
                    target, req, timeout=args.timeout, insecure=args.insecure
                )
                val = parse_ipp_response(resp)
                extra.append("  %-24s %s" % (
                    mime + ":",
                    result_line("", hs, hr, val).lstrip(": ")
                ))
            except Exception as exc:
                extra.append("  %-24s ERROR: %s" % (mime + ":", exc))

    if args.print_file:
        extra.append("")
        extra.append("Real Print-Job test")
        extra.append("-" * 66)
        try:
            with open(args.print_file, "rb") as fh:
                payload = fh.read()
            req = build_print_job(
                target["printer_uri"], args.mime,
                os.path.basename(args.print_file), 100,
                sides=args.sides
            )
            hs, hr, resp = send_ipp(
                target, req, payload=payload,
                timeout=args.timeout, insecure=args.insecure
            )
            val = parse_ipp_response(resp)
            extra.append("  File:                       %s" % args.print_file)
            extra.append("  MIME:                       %s" % args.mime)
            if args.sides:
                extra.append("  Sides:                      %s" % args.sides)
            extra.append("  Bytes:                      %d" % len(payload))
            extra.append("  Result:                     %s" % result_line("", hs, hr, val).lstrip(": "))
            message = first_value(val, "status-message")
            if isinstance(message, dict):
                message = message.get("text")
            if message:
                extra.append("  status-message:             %s" % message)
            reasons = flat_strings(all_values(val, "job-state-reasons"))
            if reasons:
                extra.append("  job-state-reasons:          %s" % ", ".join(reasons))
        except Exception as exc:
            extra.append("  ERROR: %s" % exc)

    if extra:
        extra_text = "\n".join(extra).rstrip() + "\n"
        print(extra_text, end="")
        text += extra_text

    if args.output:
        try:
            with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
            print("\nSaved report to: %s" % args.output)
        except OSError as exc:
            print("\nWARNING: could not save report: %s" % exc, file=sys.stderr)
            return 4

    if http_status >= 400 or parsed["status"] >= 0x0400:
        return 5
    return 0


if __name__ == "__main__":
    sys.exit(main())
