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

# Global state to be shared with main thread
tracker_state = {
    'config': {},
    'satellites': [],
    'selected_id': None,
    'sun_pos': {'lat': 0, 'lon': 0}
}

@app.get("/", response_class=HTMLResponse)
async def get_dashboard():
    # Fix: Ensure path is correct regardless of CWD
    # We assume CWD is 'python_tracker' when running main.py, BUT user might run from root.
    # Safe bet: check potential paths
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

    # Snapshot state safely (though GIL helps here, robust code copies)
    sats_out = []

    # We expect tracker_state['satellites'] to be a list of dicts or objects with pre-calc values
    # For performance, main loop should update this list

    return JSONResponse(content={
        "config": tracker_state['config'],
        "satellites": tracker_state['satellites']
    })

@app.get("/api/select/{norad_id}")
async def select_satellite(norad_id: int):
    tracker_state['selected_id'] = norad_id
    return {"status": "ok", "selected": norad_id}

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
