"""Points the firmware at a Wi-Fi network by patching sdkconfig.

Usage:
    python tools/configure_wifi.py                    # current WLAN, saved password
    python tools/configure_wifi.py --ssid MyNet --password secret
    python tools/configure_wifi.py --ssid MyNet --prompt

The password is never printed. sdkconfig is a generated, machine-local file, so
credentials stay out of the sources.
"""

import argparse
import getpass
import re
import subprocess
import sys
from pathlib import Path

SDKCONFIG = Path(__file__).resolve().parent.parent / "sdkconfig"


def netsh(*args):
    result = subprocess.run(["netsh", *args], capture_output=True)
    # Windows Chinese locale reports netsh output in the console code page.
    for encoding in ("utf-8", "gbk", "cp936"):
        try:
            return result.stdout.decode(encoding)
        except UnicodeDecodeError:
            continue
    return result.stdout.decode("utf-8", "replace")


def current_ssid():
    text = netsh("wlan", "show", "interfaces")
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        if key.strip().upper() == "SSID":
            return value.strip()
    return None


def saved_password(ssid):
    text = netsh("wlan", "show", "profile", f"name={ssid}", "key=clear")
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        if key.strip() in ("关键内容", "Key Content"):
            return value.strip()
    return None


def set_config(text, name, value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    line = f'CONFIG_{name}="{escaped}"'
    pattern = re.compile(rf"^CONFIG_{name}=.*$", re.MULTILINE)
    if pattern.search(text):
        return pattern.sub(line, text)
    if not text.endswith("\n"):
        text += "\n"
    return text + line + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ssid")
    parser.add_argument("--password")
    parser.add_argument("--prompt", action="store_true")
    args = parser.parse_args()

    ssid = args.ssid or current_ssid()
    if not ssid:
        print("could not detect the current SSID; pass --ssid", file=sys.stderr)
        return 1

    if args.password:
        password = args.password
    elif args.prompt:
        password = getpass.getpass(f"password for {ssid}: ")
    else:
        password = saved_password(ssid)

    if password is None:
        print(f"no saved password for {ssid}; pass --password or --prompt", file=sys.stderr)
        return 1

    if not SDKCONFIG.exists():
        print(f"{SDKCONFIG} not found; run an idf.py build once first", file=sys.stderr)
        return 1

    text = SDKCONFIG.read_text(encoding="utf-8")
    text = re.sub(r"^# CONFIG_APP_WIFI_MODE_STA is not set$", "CONFIG_APP_WIFI_MODE_STA=y",
                  text, flags=re.MULTILINE)
    text = set_config(text, "APP_WIFI_STA_SSID", ssid)
    text = set_config(text, "APP_WIFI_STA_PASSWORD", password)
    SDKCONFIG.write_text(text, encoding="utf-8")

    print(f"configured sdkconfig for \"{ssid}\" (password: {len(password)} characters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
