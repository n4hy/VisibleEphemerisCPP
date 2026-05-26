"""Web dashboard for the Python tracker (FastAPI + uvicorn).

Serves the browser UI and a JSON feed of the current satellite table. Functional
twin of the C++ WebServer's dashboard role (src/web_server.cpp).
"""
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
import uvicorn
import asyncio
import threading
import json
import logging
import os

app = FastAPI()

# Lock for thread-safe access to tracker_state
_tracker_state_lock = threading.Lock()

# Global state to be shared with main thread
tracker_state = {
    'config': {},
    'satellites': [],
    'selected_id': None,
    'sun_pos': {'lat': 0, 'lon': 0}
}

@app.get("/", response_class=HTMLResponse)
async def get_dashboard():
    # Use __file__ relative path for reliable location regardless of CWD
    script_dir = os.path.dirname(os.path.abspath(__file__))
    index_path = os.path.join(script_dir, "static", "index.html")

    if os.path.exists(index_path):
        with open(index_path, "r") as f:
            return f.read()

    # Fallback: check legacy paths
    paths = ["python_tracker/static/index.html", "static/index.html"]
    for p in paths:
        if os.path.exists(p):
            with open(p, "r") as f:
                return f.read()
    return "Error: index.html not found. CWD: " + os.getcwd()

@app.get("/api/satellites")
async def get_satellites():
    # Construct JSON matching C++ format
    # {"config": { ... }, "satellites": [ ... ]}

    # Thread-safe snapshot of state
    with _tracker_state_lock:
        config_copy = dict(tracker_state['config'])
        satellites_copy = list(tracker_state['satellites'])

    return JSONResponse(content={
        "config": config_copy,
        "satellites": satellites_copy
    })

@app.get("/api/select/{norad_id}")
async def select_satellite(norad_id: int):
    with _tracker_state_lock:
        tracker_state['selected_id'] = norad_id
    return {"status": "ok", "selected": norad_id}

def update_tracker_state(config=None, satellites=None):
    """Thread-safe update of tracker state from main loop."""
    with _tracker_state_lock:
        if config is not None:
            tracker_state['config'] = config
        if satellites is not None:
            tracker_state['satellites'] = satellites

def run_server(host="0.0.0.0", port=8080):
    # Suppress Uvicorn logging to keep terminal clean for the tracker output
    log_config = uvicorn.config.LOGGING_CONFIG
    log_config["formatters"]["access"]["fmt"] = "%(asctime)s - %(levelname)s - %(message)s"
    # uvicorn.run(app, host=host, port=port, log_config=log_config)
    uvicorn.run(app, host=host, port=port, log_level="warning")

def start_server_thread(host="0.0.0.0", port=8080):
    t = threading.Thread(target=run_server, args=(host, port), daemon=True)
    t.start()
    return t
