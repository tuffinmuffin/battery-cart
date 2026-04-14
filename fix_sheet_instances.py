#!/usr/bin/env python3
"""Restore (sheet_instances) block to all sub-schematics that kicad-skip drops."""

SHEETS = {
    r"C:\Users\beezl\OneDrive\Documents\BatteryChargerModule\CAD\BatteryMonitorModule_V9\BatteryMonitorModule\01-PowerPath.kicad_sch":
        ("/c1000001-0000-0000-0000-000000000001", "2"),
    r"C:\Users\beezl\OneDrive\Documents\BatteryChargerModule\CAD\BatteryMonitorModule_V9\BatteryMonitorModule\02-PowerSupply.kicad_sch":
        ("/c2000002-0000-0000-0000-000000000002", "3"),
    r"C:\Users\beezl\OneDrive\Documents\BatteryChargerModule\CAD\BatteryMonitorModule_V9\BatteryMonitorModule\03-MCU.kicad_sch":
        ("/c3000003-0000-0000-0000-000000000003", "4"),
    r"C:\Users\beezl\OneDrive\Documents\BatteryChargerModule\CAD\BatteryMonitorModule_V9\BatteryMonitorModule\04-IO.kicad_sch":
        ("/c4000004-0000-0000-0000-000000000004", "5"),
}

for path, (sheet_path, page) in SHEETS.items():
    try:
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
        if "(sheet_instances" in content:
            print(f"OK: {path.split(chr(92))[-1]}")
            continue
        block = f'\n\t(sheet_instances\n\t\t(path "{sheet_path}"\n\t\t\t(page "{page}")\n\t\t)\n\t)\n'
        content = content.rstrip()
        if content.endswith(")"):
            content = content[:-1] + block + ")\n"
            with open(path, "w", encoding="utf-8") as f:
                f.write(content)
            print(f"FIXED: {path.split(chr(92))[-1]}")
        else:
            print(f"ERROR: {path.split(chr(92))[-1]}")
    except FileNotFoundError:
        print(f"SKIP: {path.split(chr(92))[-1]}")
