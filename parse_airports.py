#!/usr/bin/env python3
"""Parse OpenFlights airports.dat and extract US airports for navdata.c"""

import re

import os
script_dir = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(script_dir, 'airports.dat'), 'r', encoding='utf-8') as f:
    content = f.read()

us_airports = []
for line in content.strip().split('\n'):
    parts = []
    current = ''
    in_quotes = False
    for ch in line:
        if ch == '"':
            in_quotes = not in_quotes
        elif ch == ',' and not in_quotes:
            parts.append(current.strip())
            current = ''
        else:
            current += ch
    parts.append(current.strip())

    if len(parts) >= 9 and parts[3] == 'United States':
        icao = parts[5].strip()
        iata = parts[4].strip()
        if icao and len(icao) >= 3:
            try:
                lat = float(parts[6])
                lon = float(parts[7])
                elev_str = parts[8].strip()
                elev = float(elev_str) if elev_str else 0.0
                us_airports.append({
                    'icao': icao,
                    'iata': iata,
                    'name': parts[1].strip(),
                    'city': parts[2].strip(),
                    'lat': lat,
                    'lon': lon,
                    'elev': elev
                })
            except ValueError:
                pass

print(f"US airports with ICAO: {len(us_airports)}")

# Prioritize: those with IATA codes
with_iata = [a for a in us_airports if a['iata']]
without_iata = [a for a in us_airports if not a['iata']]

print(f"With IATA (commercial): {len(with_iata)}")
print(f"Without IATA: {len(without_iata)}")

# Output C code: combine with_iata first, then fill remaining slots
# Max 128 total, 12 existing -> 116 new slots
MAX_NEW = 116
selected = []
seen_icao = set()

# Add all with IATA first (these are commercial airports)
for a in with_iata:
    if a['icao'] not in seen_icao and len(selected) < MAX_NEW:
        selected.append(a)
        seen_icao.add(a['icao'])

# Fill remaining with non-IATA airports
for a in without_iata:
    if a['icao'] not in seen_icao and len(selected) < MAX_NEW:
        selected.append(a)
        seen_icao.add(a['icao'])

print(f"\nSelected {len(selected)} airports for database")

# Output as C array initializer
print("\n// === Generated US Airport Data === //")
print("// Copy this into the apts[] array in nav_database_init()\n")

for i, a in enumerate(selected):
    # Truncate name to fit in 64 char field
    name = a['name'][:60]
    print(f'    {{ "{a["icao"]}", "{a["iata"]}", "{name}", {a["lat"]:.4f}, {a["lon"]:.4f}, {a["elev"]:.0f} }},')

# Also write to file for use
with open(os.path.join(script_dir, 'us_airports_c.txt'), 'w', encoding='utf-8') as f:
    for i, a in enumerate(selected):
        name = a['name'][:60]
        f.write(f'    {{ "{a["icao"]}", "{a["iata"]}", "{name}", {a["lat"]:.4f}, {a["lon"]:.4f}, {a["elev"]:.0f} }},\n')

print(f"\nOutput written to /tmp/us_airports_c.txt")
