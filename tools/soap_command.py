#!/usr/bin/env python3
"""soap_command.py — send a command to the RS2V server's SOAP/HTTP admin endpoint.

The RS2V Custom Server exposes its unified command system over a small
SOAP-over-HTTP endpoint (see docs/ADMIN_COMMANDS.md §3.3). This is a
dependency-light client (standard library only) for admins and AI/automation to
drive the server remotely.

Enable the endpoint in config/server.ini:

    [RemoteAdmin]
    soap_port = 27015
    password  = a-strong-shared-secret
    level     = 3

Examples:
    python3 tools/soap_command.py --host 127.0.0.1 --port 27015 \\
            --password secret "query"
    python3 tools/soap_command.py -H host -p 27015 -P secret "kick 7656... afk"

Exit status: 0 if the command reported <ok>true</ok>, 1 otherwise (auth failure,
transport error, or a command that returned false). The command's <output> is
printed to stdout.
"""

import argparse
import sys
import urllib.request
import urllib.error
from xml.sax.saxutils import escape, unescape

SOAP_TEMPLATE = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">\n'
    '  <soap:Body>\n'
    '    <ExecuteCommand>\n'
    '      <password>{password}</password>\n'
    '      <command>{command}</command>\n'
    '    </ExecuteCommand>\n'
    '  </soap:Body>\n'
    '</soap:Envelope>\n'
)


def extract_tag(xml, tag):
    """Return the decoded text of the first <tag>...</tag>, or '' if absent."""
    open_tag = "<" + tag
    s = xml.find(open_tag)
    if s == -1:
        return ""
    gt = xml.find(">", s)
    if gt == -1:
        return ""
    close = "</" + tag + ">"
    e = xml.find(close, gt + 1)
    if e == -1:
        return ""
    return unescape(xml[gt + 1:e])


def main():
    ap = argparse.ArgumentParser(description="Send a command to the RS2V SOAP admin endpoint.")
    ap.add_argument("-H", "--host", default="127.0.0.1", help="server host (default: 127.0.0.1)")
    ap.add_argument("-p", "--port", type=int, required=True, help="RemoteAdmin.soap_port")
    ap.add_argument("-P", "--password", required=True, help="RemoteAdmin.password")
    ap.add_argument("-t", "--timeout", type=float, default=5.0, help="request timeout seconds")
    ap.add_argument("command", help="command line to execute, e.g. 'status' or 'kick 7656... afk'")
    args = ap.parse_args()

    body = SOAP_TEMPLATE.format(
        password=escape(args.password),
        command=escape(args.command),
    ).encode("utf-8")

    url = "http://{}:{}/".format(args.host, args.port)
    req = urllib.request.Request(
        url, data=body,
        headers={
            "Content-Type": "text/xml; charset=utf-8",
            "SOAPAction": "ExecuteCommand",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            xml = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # Auth/usage failures still carry a SOAP fault body.
        xml = e.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as e:
        print("transport error: {}".format(e), file=sys.stderr)
        return 2

    ok = extract_tag(xml, "ok").strip().lower() == "true"
    output = extract_tag(xml, "output")
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
