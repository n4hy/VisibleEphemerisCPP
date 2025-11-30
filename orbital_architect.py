#!/usr/bin/env python3
import os
import sys
import glob
import re

# --- CONFIGURATION ---
CACHE_DIR = "tle_cache"
CONFIG_FILE = "config.yaml"

# --- ANSI COLORS ---
C_RESET  = "\033[0m"
C_CYAN   = "\033[96m"
C_GREEN  = "\033[92m"
C_YELLOW = "\033[93m"
C_RED    = "\033[91m"
C_BOLD   = "\033[1m"
C_CLS    = "\033[2J\033[H"

class Satellite:
    def __init__(self, name, l1, l2, source_file):
        self.name = name.strip()
        self.l1 = l1.strip()
        self.l2 = l2.strip()
        # Safe ID extraction
        try:
            self.id = int(l1[2:7])
        except:
            self.id = 0
        self.source = source_file

    def __repr__(self):
        return f"[{self.id}] {self.name}"

class OrbitalArchitect:
    def __init__(self):
        self.catalog = {} # ID -> Satellite
        self.selected = {} # ID -> Satellite
        self.config = {}
        self.load_config()
        self.load_cache()

    def log(self, msg, color=C_RESET):
        print(f"{color}{msg}{C_RESET}")

    def load_config(self):
        self.config = {
            'lat': 0.0, 'lon': 0.0, 'alt': 0.0,
            'min_el': 0.0, 'max_apo': -1.0,
            'group_selection': 'active',
            'show_all_visible': False,
            'sat_selection': ''
        }
        if not os.path.exists(CONFIG_FILE): return
        
        with open(CONFIG_FILE, 'r') as f:
            for line in f:
                if ':' in line:
                    parts = line.split(':', 1)
                    key = parts[0].strip()
                    val = parts[1].strip()
                    
                    if val.lower() == 'true': val = True
                    elif val.lower() == 'false': val = False
                    else:
                        try: val = float(val)
                        except: pass
                    self.config[key] = val

    def save_config(self):
        with open(CONFIG_FILE, 'w') as f:
            for k, v in self.config.items():
                if isinstance(v, bool):
                    f.write(f"{k}: {'true' if v else 'false'}\n")
                else:
                    f.write(f"{k}: {v}\n")
        self.log(f"✅ Configuration saved to {CONFIG_FILE}", C_GREEN)

    def load_cache(self):
        if not os.path.exists(CACHE_DIR):
            self.log(f"❌ Error: {CACHE_DIR} not found. Run './VisibleEphemeris --refresh' first!", C_RED)
            sys.exit(1)
        
        files = glob.glob(f"{CACHE_DIR}/*.txt")
        self.log(f"Scanning {len(files)} cache files...", C_CYAN)
        
        count = 0
        for filepath in files:
            filename = os.path.basename(filepath)
            # Skip the user_defined file to prevent reading back what we write
            if filename == "user_defined.txt": continue 
            
            try:
                with open(filepath, 'r') as f:
                    lines = f.readlines()
            except: continue
                
            # TLE Parser (3 line format)
            i = 0
            while i < len(lines):
                line = lines[i].strip()
                if not line: 
                    i += 1
                    continue
                
                # Check for Name line
                # (Sometimes TLEs are 2 lines, but Celestrak is usually 3)
                if i + 2 < len(lines):
                    l1 = lines[i+1].strip()
                    l2 = lines[i+2].strip()
                    if l1.startswith('1 ') and l2.startswith('2 '):
                        sat = Satellite(line, l1, l2, filename)
                        self.catalog[sat.id] = sat
                        count += 1
                        i += 3
                        continue
                i += 1
        self.log(f"✅ Indexed {len(self.catalog)} unique satellites.", C_GREEN)

    def header(self):
        print(C_CLS)
        print(f"{C_CYAN}{C_BOLD}   ORBITAL ARCHITECT v1.0   {C_RESET}")
        print(f"{C_CYAN}   ======================   {C_RESET}")
        print(f"   Observer:       {self.config.get('lat')} , {self.config.get('lon')}")
        print(f"   Selected Sats:  {len(self.selected)}")
        print(f"   Radio Mode:     {'ON' if self.config.get('show_all_visible') else 'OFF'}")
        print(f"{C_CYAN}   ----------------------   {C_RESET}")

    def menu_search(self):
        while True:
            self.header()
            print(f"   [ Selected Fleet: {len(self.selected)} ]")
            if len(self.selected) > 0:
                # Show first few selected
                preview = list(self.selected.values())
                for s in preview[:5]: print(f"     - {s.name} ({s.id})")
                if len(preview) > 5: print(f"     ... and {len(preview)-5} more")
            
            print("")
            query = input(f"{C_YELLOW}Search (Name or ID) or 'DONE': {C_RESET}").strip().upper()
            
            if query == "": continue
            if query == "DONE" or query == "EXIT": break
            if query == "CLEAR": 
                self.selected.clear()
                continue
            if query == "ALL":
                print(f"{C_RED}Warning: Selecting ALL {len(self.catalog)} satellites is not recommended.{C_RESET}")
                continue

            # Search Logic
            matches = []
            # ID search
            if query.isdigit():
                 qid = int(query)
                 if qid in self.catalog: matches.append(self.catalog[qid])
            
            # Name search
            if not matches:
                matches = [s for s in self.catalog.values() if query in s.name.upper()]
            
            if not matches:
                input(f"{C_RED}No matches found.{C_RESET} Press Enter...")
                continue
            
            # Show matches
            print(f"\n   Found {len(matches)} matches:")
            limit = 20
            for idx, sat in enumerate(matches[:limit]):
                chk = f"{C_GREEN}[x]{C_RESET}" if sat.id in self.selected else "[ ]"
                print(f"   {idx+1}. {chk} {sat.name}  \t(ID: {sat.id})")
            
            if len(matches) > limit: print(f"   ... {len(matches)-limit} more hidden ...")
            
            cmd = input(f"\n{C_YELLOW}Enter numbers to toggle (e.g. 1,3-5), 'A'll shown, or Enter to skip: {C_RESET}").strip().upper()
            if cmd == 'A':
                for s in matches: self.selected[s.id] = s
            elif cmd:
                # Range Parser
                parts = cmd.split(',')
                for p in parts:
                    try:
                        if '-' in p:
                            start, end = map(int, p.split('-'))
                            for i in range(start, min(end+1, len(matches)+1)):
                                s = matches[i-1]
                                if s.id in self.selected: del self.selected[s.id]
                                else: self.selected[s.id] = s
                        else:
                            idx = int(p)
                            if 1 <= idx <= len(matches):
                                s = matches[idx-1]
                                if s.id in self.selected: del self.selected[s.id]
                                else: self.selected[s.id] = s
                    except: pass

    def menu_config(self):
        while True:
            self.header()
            print("   1. Set Latitude")
            print("   2. Set Longitude")
            print("   3. Set Min Elevation")
            print("   4. Toggle Radio Mode (Show All / Invisible)")
            print("   0. Back")
            choice = input(f"\n{C_YELLOW}Choice: {C_RESET}")
            
            if choice == '0': break
            if choice == '1': 
                try: self.config['lat'] = float(input("Lat: "))
                except: pass
            if choice == '2': 
                try: self.config['lon'] = float(input("Lon: "))
                except: pass
            if choice == '3': 
                try: self.config['min_el'] = float(input("Min El: "))
                except: pass
            if choice == '4': 
                self.config['show_all_visible'] = not self.config.get('show_all_visible', False)

    def deploy(self):
        self.header()
        if not self.selected:
            print(f"{C_RED}No satellites selected! Cannot deploy.{C_RESET}")
            input("Press Enter...")
            return

        group_name = input(f"{C_YELLOW}Name this group (default: user_defined): {C_RESET}").strip()
        if not group_name: group_name = "user_defined"
        # Sanitize filename
        group_name = "".join([c for c in group_name if c.isalnum() or c in ('-','_')])
        
        # 1. Write TLE File
        filename = f"{CACHE_DIR}/{group_name}.txt"
        try:
            with open(filename, 'w') as f:
                for s in self.selected.values():
                    f.write(f"{s.name}\n{s.l1}\n{s.l2}\n")
            print(f"\n{C_GREEN}>> TLE Group written to {filename}{C_RESET}")
        except Exception as e:
            print(f"{C_RED}Error writing TLE file: {e}{C_RESET}")
            input("Press Enter...")
            return

        # 2. Update Config
        self.config['group_selection'] = group_name
        self.config['sat_selection'] = "" # Clear specific sat selection to use group
        self.save_config()
        
        print(f"{C_GREEN}>> Config updated to target group '{group_name}'{C_RESET}")
        print(f"\n{C_BOLD}MISSION DEPLOYED.{C_RESET}")
        print("Run './VisibleEphemeris' to launch.")
        sys.exit(0)

    def run(self):
        while True:
            self.header()
            print(f"   1. {C_BOLD}Search & Select Satellites{C_RESET}")
            print(f"   2. Station Configuration")
            print(f"   3. {C_GREEN}{C_BOLD}DEPLOY MISSION{C_RESET}")
            print(f"   0. Exit")
            
            choice = input(f"\n{C_YELLOW}Command: {C_RESET}")
            
            if choice == '1': self.menu_search()
            if choice == '2': self.menu_config()
            if choice == '3': self.deploy()
            if choice == '0': sys.exit(0)

if __name__ == "__main__":
    try:
        app = OrbitalArchitect()
        app.run()
    except KeyboardInterrupt:
        print("\nAborted.")
