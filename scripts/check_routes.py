#!/usr/bin/env python3
# Report every UI->backend route name and whether the backend registers it.
#
# WHY: the bulk tool rename was a plain string replace, so it also rewrote route
# names ("plan.get" -> "plan_get") that the UI and the backend must spell the
# same. A route that no longer matches fails at runtime with "unknown method",
# which is silent until a user clicks the button.
#
# This script extracts every reg("...") from ui_api.cpp and every
# call_async("...") / call("...") from ui_app.cpp and prints the mismatches.
import io
import os
import re
import sys


def read(path):
    return io.open(path, encoding="utf-8").read()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    api = read(os.path.join(root, "src/ui/ui_api.cpp"))
    app = read(os.path.join(root, "src/ui/ui_app.cpp"))

    registered = set(re.findall(r'reg\(\s*"([^"]+)"', api))
    called = set(re.findall(r'call_async\(\s*"([^"]+)"', app))
    called |= set(re.findall(r'\bcall\(\s*"([^"]+)"', app))

    missing = sorted(called - registered)
    unused = sorted(registered - called)
    print("registered: %d, called: %d" % (len(registered), len(called)))
    if missing:
        print("\nCALLED BUT NOT REGISTERED (runtime 'unknown method'):")
        for m in missing:
            print("  " + m)
    if unused:
        print("\nregistered but never called (may be intentional):")
        for u in unused:
            print("  " + u)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
