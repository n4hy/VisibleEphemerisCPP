#include "web_server.hpp"
#include "logger.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h> 
#include <algorithm>

namespace ve {

    void sendAll(int socket, const std::string& header, const std::string& body) {
        size_t total = header.length();
        size_t sent = 0;
        while(sent < total) {
            ssize_t bytes = send(socket, header.c_str() + sent, total - sent, MSG_NOSIGNAL);
            if (bytes <= 0) return; 
            sent += bytes;
        }
        total = body.length();
        sent = 0;
        const size_t CHUNK_SIZE = 16384; 
        while(sent < total) {
            size_t remaining = total - sent;
            size_t to_send = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
            ssize_t bytes = send(socket, body.c_str() + sent, to_send, MSG_NOSIGNAL);
            if (bytes <= 0) return; 
            sent += bytes;
        }
    }

    const char* DASHBOARD_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Visible Ephemeris Dashboard</title>
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.7.1/dist/leaflet.css" />
    <script src="https://unpkg.com/leaflet@1.7.1/dist/leaflet.js"></script>
    <style>
        body, html { margin: 0; padding: 0; height: 100%; width: 100%; background: #111; color: #ddd; font-family: monospace; overflow: hidden; }
        .container { display: flex; width: 100%; height: 100%; }
        .sidebar { width: 40%; min-width: 450px; background: #1a1a1a; border-right: 1px solid #333; display: flex; flex-direction: column; }
        .header { padding: 15px; background: #222; border-bottom: 1px solid #444; display:flex; justify-content:space-between; align-items:center;}
        .header h2 { margin: 0; color: #4da6ff; font-size: 18px; }
        .table-wrap { flex-grow: 1; overflow-y: auto; }
        table { width: 100%; border-collapse: collapse; font-size: 12px; table-layout: fixed; }
        th { position: sticky; top: 0; background: #333; color: #fff; padding: 8px; text-align: left; cursor: pointer; user-select: none; }
        th:hover { background: #444; }
        th.sort-asc { color: #00ffff; border-bottom: 2px solid #00ffff; }
        th.sort-desc { color: #ff00ff; border-bottom: 2px solid #ff00ff; }
        td { padding: 6px 8px; border-bottom: 1px solid #2a2a2a; cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        tr:hover { background: #333; }
        tr.active { background: #2c3e50; border-left: 4px solid #4da6ff; }
        .map-pane { flex-grow: 1; position: relative; background: #000; }
        #map, #skyplot { width: 100%; height: 100%; position: absolute; top: 0; left: 0; }
        #skyplot { display: none; background: #000; }
        .control-btn { padding: 5px 10px; background: #333; border: 1px solid #555; color: #fff; cursor: pointer; margin-left:5px; }
        .vis-YES { color: #0f0; font-weight: bold; } .vis-DAY { color: #ff0; } .vis-NO { color: #0ff; }
        .house-icon { font-size: 24px; text-align: center; text-shadow: 2px 2px 4px #000; }
        @keyframes flash-yellow { 0% { fill-opacity: 1; fill: #ffff00; } 50% { fill-opacity: 0.2; fill: #ffff00; } 100% { fill-opacity: 1; fill: #ffff00; } }
        @keyframes flash-fast { 0% { fill-opacity: 1; fill: #ffff00; } 50% { fill-opacity: 0; fill: #ff0000; } 100% { fill-opacity: 1; fill: #ffff00; } }
        .flare-near { animation: flash-yellow 1s infinite; fill: #ffff00 !important; color: #ffff00 !important; }
        .flare-hit { animation: flash-fast 0.2s infinite; fill: #ffff00 !important; color: #ffff00 !important; }
    </style>
</head>
<body>
    <div class="container">
        <div class="sidebar">
            <div class="header">
                <div><h2>VISIBLE EPHEMERIS</h2><div id="status">Connecting...</div></div>
                <button class="control-btn" onclick="toggleView()">MAP / SKY</button>
            </div>
            <div class="table-wrap">
                <table>
                    <thead>
                        <tr>
                            <th onclick="sortBy('n')" id="th-n">Name</th>
                            <th onclick="sortBy('a')" id="th-a">Az</th>
                            <th onclick="sortBy('e')" id="th-e">El</th>
                            <th onclick="sortBy('next')" id="th-next">Next Event</th>
                            <th onclick="sortBy('v')" id="th-v">Vis</th>
                        </tr>
                    </thead>
                    <tbody id="sat-list"></tbody>
                </table>
            </div>
        </div>
        <div class="map-pane"><div id="map"></div><canvas id="skyplot"></canvas></div>
    </div>
    <script>
        var map = L.map('map', {zoomControl: false}).setView([0, 0], 2);
        L.control.zoom({position: 'topright'}).addTo(map); 
        L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {maxZoom: 19}).addTo(map);
        
        var currentView='map', lastData=[], selectedId=null, houseMarker, initialZoomDone=false;
        var selectedFootprint=null, terminatorPoly=null;
        var sortCol = 'e', sortAsc = false;

        var markers = {}; var polylines = {};

        function toggleView() { 
            currentView = (currentView==='map') ? 'sky' : 'map'; 
            document.getElementById('map').style.display = (currentView==='map')?'block':'none';
            document.getElementById('skyplot').style.display = (currentView==='sky')?'block':'none';
            if (currentView === 'sky') resizeCanvas();
            else map.invalidateSize();
        }
        
        var canvas = document.getElementById('skyplot'); 
        var ctx = canvas.getContext('2d');
        
        function resizeCanvas() { 
            if (canvas.parentElement) {
                canvas.width = canvas.parentElement.clientWidth; 
                canvas.height = canvas.parentElement.clientHeight; 
            }
        }
        window.addEventListener('resize', resizeCanvas);

        function sortBy(col) {
            if (sortCol === col) sortAsc = !sortAsc; else { sortCol = col; sortAsc = true; }
            updateHeaders(); renderTable();
        }
        function updateHeaders() {
            ['n','a','e','next','v'].forEach(c => { var el=document.getElementById('th-'+c); if(el) el.className=''; });
            var active = document.getElementById('th-'+sortCol); if(active) active.className = sortAsc ? 'sort-asc' : 'sort-desc';
        }

        function computeTerminator(sunLat, sunLon) {
            var latLngs = [], sunRad = Math.PI/180.0;
            if (Math.abs(sunLat) < 0.1) sunLat = (sunLat >= 0 ? 0.1 : -0.1);
            var tanSun = Math.tan(sunLat * sunRad);
            for(var i=-180; i<=180; i+=5) {
                var deltaL = (i - sunLon) * sunRad;
                var lat = Math.atan(-Math.cos(deltaL) / tanSun) * 180.0/Math.PI;
                if(lat > 85) lat = 85; if(lat < -85) lat = -85;
                latLngs.push([lat, i]);
            }
            var closeLat = (sunLat > 0) ? -90 : 90;
            latLngs.push([closeLat, 180]); latLngs.push([closeLat, -180]);
            return latLngs;
        }

        function updateSats() {
            fetch('/api/satellites').then(r=>r.json()).then(d => {
                lastData = d.satellites || [];
                document.getElementById('status').innerText = "Live: " + lastData.length;
                renderTable();
                renderMap(d.config);
            }).catch(e => console.error("Data fetch error:", e));
        }

        function renderTable() {
            if (!lastData) return;
            lastData.sort((a,b) => {
                var vA = a[sortCol], vB = b[sortCol];
                if (typeof vA === 'string') { vA = vA.toLowerCase(); vB = vB.toLowerCase(); }
                if (vA < vB) return sortAsc ? -1 : 1;
                if (vA > vB) return sortAsc ? 1 : -1;
                return 0;
            });
            var html = '';
            lastData.forEach(s => {
                var cls = (s.id===selectedId) ? 'active' : '';
                var visCls = 'vis-' + s.v;
                var displayName = s.n;
                if (s.f > 0) {
                    visCls = 'vis-DAY';
                    displayName += " (F)";
                }
                html += `<tr class="${cls}" onclick="selectSat(${s.id})">
                    <td>${displayName}</td><td>${s.a.toFixed(1)}</td><td>${s.e.toFixed(1)}</td><td>${s.next}</td><td class="${visCls}">${s.v}</td></tr>`;
            });
            document.getElementById('sat-list').innerHTML = html;
            updateHeaders();
        }
        
        function selectSat(id) { 
            selectedId = id; 
            fetch('/api/select/' + id);
            if(currentView==='map') { 
                var s = lastData.find(x => x.id === id);
                if(s) map.panTo([s.lat, s.lon]); 
            } 
            renderTable(); 
        }

        function renderMap(config) {
            if(!config) return;
            
            // --- SKYPLOT ---
            if(currentView === 'sky') {
                if(canvas.width === 0) resizeCanvas();
                ctx.fillStyle='#000'; ctx.fillRect(0, 0, canvas.width, canvas.height);
                var cx=canvas.width/2; var cy=canvas.height/2; var r=Math.min(cx, cy)*0.9;
                ctx.strokeStyle='#008800'; ctx.lineWidth=1.5;
                ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2*Math.PI); ctx.stroke();
                ctx.beginPath(); ctx.arc(cx, cy, r*0.66, 0, 2*Math.PI); ctx.stroke();
                ctx.beginPath(); ctx.arc(cx, cy, r*0.33, 0, 2*Math.PI); ctx.stroke();
                for(var ang=0; ang<360; ang+=45) {
                    var rad = (ang - 90) * (Math.PI/180);
                    ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx+r*Math.cos(rad), cy+r*Math.sin(rad)); ctx.stroke();
                }
                ctx.fillStyle='#00ff00'; ctx.font='14px monospace'; 
                ctx.fillText('N', cx-5, cy-r-5); ctx.fillText('E', cx+r+5, cy+5);
                
                lastData.forEach(s => {
                    if(s.e < 0) return;
                    var dist = r * (90.0 - s.e) / 90.0;
                    var rad = (s.a - 90.0) * (Math.PI/180.0);
                    var x = cx + dist * Math.cos(rad);
                    var y = cy + dist * Math.sin(rad);
                    if(s.id===selectedId) {
                        var t=Date.now(); var pr=8+4*Math.sin(t*0.005); 
                        ctx.save(); ctx.beginPath(); ctx.arc(x, y, pr, 0, 2*Math.PI); 
                        ctx.strokeStyle='#ff00ff'; ctx.lineWidth=2; ctx.stroke(); ctx.restore();
                    }
                    var col = (s.v==="YES") ? "#0f0" : ((s.v==="DAY")?"#ff0":"#0ff");
                    if (s.f > 0) {
                        col = "#ffff00";
                        var t_ms = Date.now();
                        var period = (s.f === 2) ? 200 : 1000;
                        if ((Math.floor(t_ms / (period/2)) % 2) === 0) col = "#444";
                        else col = "#ffff00";
                    }
                    ctx.fillStyle = col; ctx.beginPath(); ctx.arc(x,y,5,0,2*Math.PI); ctx.fill();
                    ctx.fillStyle='#fff'; ctx.fillText(s.n, x+8, y+3);
                });
                return;
            }
            
            // --- MAP ---
            if(houseMarker) {
                houseMarker.setLatLng([config.lat, config.lon]);
            } else {
                houseMarker = L.marker([config.lat, config.lon], {icon: L.divIcon({html:'🏠', className:'house-icon'})}).addTo(map).bindPopup("Observer");
            }

            if(!initialZoomDone && config.max_apo > 0) { map.setView([config.lat, config.lon], 3); initialZoomDone=true; }

            if(config.sun_lat !== undefined) {
                var pts = computeTerminator(config.sun_lat, config.sun_lon);
                if(terminatorPoly) terminatorPoly.setLatLngs(pts);
                else terminatorPoly = L.polygon(pts, {color:'transparent', fillColor:'#000', fillOpacity:0.4}).addTo(map);
            }

            // FOOTPRINT LOGIC (Strict)
            var selSat = lastData.find(s => s.id === selectedId);
            
            if(selectedFootprint && (!selSat || selSat.id !== selectedId)) { 
                map.removeLayer(selectedFootprint); 
                selectedFootprint = null; 
            }
            
            if(selSat) {
                var alt = selSat.apo; // No fallback
                if (alt && alt > 0) {
                    var rMeters = 6378137 * Math.acos(6378.137 / (6378.137 + alt));
                    // Log to console to confirm drawing
                    console.log("Footprint: " + selSat.n + " Alt:" + alt + " R:" + rMeters);
                    
                    if(selectedFootprint) { 
                        selectedFootprint.setLatLng([selSat.lat, selSat.lon]); 
                        selectedFootprint.setRadius(rMeters); 
                        selectedFootprint.bringToFront();
                    } else { 
                        selectedFootprint = L.circle([selSat.lat, selSat.lon], {
                            radius: rMeters, 
                            color:'#FFFF00', // Yellow
                            weight:2, 
                            fillColor:'#FFFF00', 
                            fillOpacity:0.5, 
                            dashArray:'5,5'
                        }).addTo(map); 
                        selectedFootprint.bringToFront();
                    }
                } else if (selectedFootprint) {
                    // If alt goes invalid (decay?), remove it
                    map.removeLayer(selectedFootprint);
                    selectedFootprint = null;
                }
            }

            var currentIds = new Set();
            lastData.forEach(s => {
                currentIds.add(s.id);
                if(markers[s.id]) { markers[s.id].setLatLng([s.lat, s.lon]); }
                else { markers[s.id]=L.circleMarker([s.lat, s.lon], {color:'#0f0', radius:6, weight:1, fillColor:'#0f0', fillOpacity:0.9}).addTo(map).on('click', ()=>selectSat(s.id)); }
                
                var color = (s.v==="YES") ? "#00ff00" : ((s.v==="DAY")?"#ffff00":"#00ffff");
                var cls = "";
                if(s.f > 0) {
                     color = "#ffff00";
                     cls = (s.f === 2) ? "flare-hit" : "flare-near";
                }
                markers[s.id].setStyle({color:color, fillColor:color, className: cls});
                // Force class update for SVG
                if(markers[s.id].getElement()) {
                    markers[s.id].getElement().setAttribute('class', 'leaflet-interactive ' + cls);
                }

                if(s.trail) { if(polylines[s.id]) polylines[s.id].setLatLngs(s.trail); else polylines[s.id]=L.polyline(s.trail, {color:'#0ff', weight:2, opacity:0.7, dashArray: '5,5'}).addTo(map); }
            });
            for(var id in markers) if(!currentIds.has(parseInt(id))) { map.removeLayer(markers[id]); delete markers[id]; }
            for(var id in polylines) if(!currentIds.has(parseInt(id))) { map.removeLayer(polylines[id]); delete polylines[id]; }
        }

        setInterval(updateSats, 1000);
    </script>
</body>
</html>
)HTML";

    // ... (WebServer Implementation with 3-arg constructor same as previous bundle) ...
    
    WebServer::WebServer(int port, TLEManager& tle_mgr, bool builder_mode) 
        : port_(port), server_fd_(-1), running_(false), tle_mgr_(tle_mgr), builder_mode_(builder_mode) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in address; address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(port_);
        bind(server_fd_, (struct sockaddr*)&address, sizeof(address));
        listen(server_fd_, 10);
        std::cout << "[INFO] WebServer started on port " << port_ << " (Mode: " << (builder_mode ? "BUILDER" : "TRACKER") << ")" << std::endl;
    }

    WebServer::~WebServer() { stop(); }
    void WebServer::start() { running_ = true; server_thread_ = std::thread(&WebServer::serverLoop, this); }
    void WebServer::runBlocking() { running_ = true; serverLoop(); }
    void WebServer::stop() { running_ = false; if (server_fd_ >= 0) { shutdown(server_fd_, SHUT_RDWR); close(server_fd_); server_fd_ = -1; } if(server_thread_.joinable()) server_thread_.join(); }
    void WebServer::updateData(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_json_data_ = buildJson(rows, raw_sats, config);
        last_known_config_ = config;
    }
    bool WebServer::hasPendingConfig() { std::lock_guard<std::mutex> lock(config_mutex_); return config_changed_; }
    AppConfig WebServer::popPendingConfig() { std::lock_guard<std::mutex> lock(config_mutex_); config_changed_ = false; return pending_config_; }

    int WebServer::getSelectedNoradId() const {
        return selected_norad_id_.load();
    }

    std::string WebServer::buildJson(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config) {
        std::stringstream ss;
        Geodetic sun = VisibilityCalculator::getSunPositionGeo(Clock::now());
        ss << "{\"config\":{\"lat\":" << config.lat << ",\"lon\":" << config.lon << ",\"min_el\":" << config.min_el 
           << ",\"max_apo\":" << config.max_apo << ",\"show_all\":" << (config.show_all_visible ? "true" : "false") 
           << ",\"groups\":\"" << config.group_selection << "\","
           << "\"sun_lat\":" << sun.lat_deg << ",\"sun_lon\":" << sun.lon_deg << "},";
        ss << "\"satellites\":[";
        for(size_t i=0; i<rows.size(); ++i) {
            const auto& r = rows[i];
            ss << "{\"id\":" << r.norad_id << ",\"n\":\"" << r.name << "\",\"lat\":" << r.lat << ",\"lon\":" << r.lon 
               << ",\"a\":" << r.az << ",\"e\":" << r.el << ",\"v\":\"" << (r.state==VisibilityCalculator::State::VISIBLE?"YES":(r.state==VisibilityCalculator::State::DAYLIGHT?"DAY":"NO")) 
               << "\",\"next\":\"" << r.next_event << "\",\"apo\":" << r.apogee << ",\"f\":" << r.flare_status << "}";
            if(i < rows.size()-1) ss << ",";
        }
        ss << "]}"; 
        return ss.str();
    }

    std::string WebServer::urlDecode(const std::string& str) {
        std::string ret; for (size_t i=0; i < str.length(); i++) { if(str[i] != '%'){ if(str[i] == '+') ret += ' '; else ret += str[i]; } else { int ii; sscanf(str.substr(i + 1, 2).c_str(), "%x", &ii); ret += static_cast<char>(ii); i += 2; } } return ret;
    }
    std::map<std::string, std::string> WebServer::parseQuery(const std::string& query) {
        std::map<std::string, std::string> data; std::stringstream ss(query); std::string item;
        while (std::getline(ss, item, '&')) { size_t pos = item.find('='); if (pos != std::string::npos) data[item.substr(0, pos)] = urlDecode(item.substr(pos + 1)); }
        return data;
    }
    void WebServer::handleRequest(int client_socket, const std::string& request) {
        std::string method, path; std::stringstream ss(request); ss >> method >> path;
        std::string clean_path = path.substr(0, path.find('?'));
        auto params = parseQuery(path.substr(path.find('?') + 1));

        // Stub for builder HTML since we removed it from this file to focus on tracker fix
        // In a real full merge, builder HTML would be here.
        if (builder_mode_) {
             std::string body = "<html><body><h1>Builder Mode Active</h1><p>Run ./orbital_architect.py for advanced planning.</p></body></html>"; 
             std::string header = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " + std::to_string(body.length()) + "\r\n\r\n";
             sendAll(client_socket, header, body);
             return;
        }

        if (clean_path == "/api/satellites") {
            std::lock_guard<std::mutex> lock(data_mutex_);
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache, no-store\r\nContent-Length: " + std::to_string(current_json_data_.length()) + "\r\n\r\n" + current_json_data_;
            send(client_socket, resp.c_str(), resp.length(), MSG_NOSIGNAL);
        } else if (clean_path.rfind("/api/select/", 0) == 0) {
            try {
                std::string id_str = clean_path.substr(12);
                int norad_id = std::stoi(id_str);
                selected_norad_id_ = norad_id;
                std::string body = "{\"status\":\"ok\"}";
                std::string header = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.length()) + "\r\n\r\n";
                sendAll(client_socket, header, body);
            } catch (...) {
                std::string body = "{\"status\":\"error\", \"message\":\"Invalid NORAD ID\"}";
                std::string header = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.length()) + "\r\n\r\n";
                sendAll(client_socket, header, body);
            }
        } else {
            std::string body = DASHBOARD_HTML;
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-cache, no-store\r\nContent-Length: " + std::to_string(body.length()) + "\r\n\r\n" + body;
            send(client_socket, resp.c_str(), resp.length(), MSG_NOSIGNAL);
        }
    }
    void WebServer::serverLoop() {
        while (running_) {
            int new_socket = accept(server_fd_, NULL, NULL);
            if (new_socket >= 0) {
                char buffer[4096]; int bytes = read(new_socket, buffer, 4096);
                if (bytes > 0) { std::string request(buffer, bytes); handleRequest(new_socket, request); }
                shutdown(new_socket, SHUT_WR); char drain[128]; while(read(new_socket, drain, sizeof(drain)) > 0); close(new_socket);
            } else { if (!running_) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }
    }
}
