"""Configuration loader/saver for the Python tracker.

Reads and writes config.yaml (YAML) into a settings dict, falling back to a
built-in defaults table for any missing key. The functional twin of the C++
ConfigManager (include/config_manager.hpp).
"""
import yaml
import os

class ConfigManager:
    def __init__(self, filepath="config.yaml"):
        self.filepath = filepath
        self.config = {
            'lat': 0.0,
            'lon': 0.0,
            'alt': 0.0,
            'min_el': 0.0,
            'max_apo': -1,
            'max_sats': 100,
            'visible_only': False,  # False = show all satellites, True = optical mode only
            'show_all_visible': False,  # Legacy compatibility
            'group_selection': "active",
            'trail_length_mins': 5,
            'delta_t': 1.0
        }
        self.load()

    def load(self):
        if not os.path.exists(self.filepath):
            return

        try:
            with open(self.filepath, 'r') as f:
                data = yaml.safe_load(f)
                if data:
                    self.config.update(data)
        except Exception as e:
            print(f"Error loading config: {e}")

    def get(self, key, default=None):
        return self.config.get(key, default)

    def save(self, new_config_values=None):
        if new_config_values:
            self.config.update(new_config_values)

        try:
            with open(self.filepath, 'w') as f:
                yaml.dump(self.config, f, default_flow_style=False)
            print(f"Configuration saved to {self.filepath}")
        except Exception as e:
            print(f"Error saving config: {e}")
