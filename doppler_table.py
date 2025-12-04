#!/usr/bin/env python3
import csv
import os

FILENAME = "frequencies.csv"

def load_table():
    data = {}
    if os.path.exists(FILENAME):
        with open(FILENAME, 'r') as f:
            reader = csv.reader(f)
            for row in reader:
                if len(row) >= 4 and not row[0].startswith('#'):
                    data[row[0].upper()] = {'ul': row[1], 'dl': row[2], 'mode': row[3]}
    return data

def save_table(data):
    with open(FILENAME, 'w') as f:
        writer = csv.writer(f)
        writer.writerow(['# Name', 'Uplink(Hz)', 'Downlink(Hz)', 'Mode'])
        for name, info in data.items():
            writer.writerow([name, info['ul'], info['dl'], info['mode']])
    print(f"Saved to {FILENAME}")

def main():
    print("=== Doppler Table Manager ===")
    data = load_table()

    while True:
        print(f"\nLoaded {len(data)} entries.")
        print("1. Add/Edit Entry")
        print("2. Remove Entry")
        print("3. List Entries")
        print("4. Save & Exit")
        print("0. Cancel")

        choice = input("Choice: ")

        if choice == '1':
            name = input("Satellite Name: ").strip().upper()
            ul = input("Uplink Freq (Hz): ").strip()
            dl = input("Downlink Freq (Hz): ").strip()
            mode = input("Mode (FM/SSB/CW): ").strip().upper()
            data[name] = {'ul': ul, 'dl': dl, 'mode': mode}
        elif choice == '2':
            name = input("Satellite Name to remove: ").strip().upper()
            if name in data: del data[name]
        elif choice == '3':
            print("\n--- Current Table ---")
            for name, info in data.items():
                print(f"{name}: UL {info['ul']} / DL {info['dl']} [{info['mode']}]")
        elif choice == '4':
            save_table(data)
            break
        elif choice == '0':
            break

if __name__ == "__main__":
    main()
