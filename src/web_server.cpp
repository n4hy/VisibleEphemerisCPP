#include "web_server.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h> 

namespace ve {

    const char* HTML_CONTENT = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Visible Ephemeris Command</title>
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.7.1/dist/leaflet.css" />
    <script src="https://unpkg.com/leaflet@1.7.1/dist/leaflet.js"></script>
    <style>
        body, html { margin: 0; padding: 0; height: 100%; width: 100%; background: #111; color: #ddd; font-family: 'Courier New', monospace; overflow: hidden; }
        .container { display: flex; width: 100%; height: 100%; }
        .sidebar { width: 40%; min-width: 450px; background: #1a1a1a; border-right: 1px solid #333; display: flex; flex-direction: column; }
        .header { padding: 15px; background: #222; border-bottom: 1px solid #444; display:flex; justify-content:space-between; align-items:center;}
        .header h2 { margin: 0; color: #4da6ff; font-size: 18px; }
        #status { font-size: 11px; color: #888; margin-top: 5px; }
        
        /* FIXED LAYOUT TABLE CSS */
        .table-wrap { flex-grow: 1; overflow-y: auto; overflow-x: auto; scrollbar-width: thin; scrollbar-color: #4da6ff #222; }
        .table-wrap::-webkit-scrollbar { width: 10px; height: 10px; }
        .table-wrap::-webkit-scrollbar-track { background: #222; }
        .table-wrap::-webkit-scrollbar-thumb { background: #444; border-radius: 4px; }
        .table-wrap::-webkit-scrollbar-thumb:hover { background: #4da6ff; }
        table { width: 100%; min-width: 650px; border-collapse: collapse; font-size: 12px; table-layout: fixed; }
        th { position: sticky; top: 0; background: #333; color: #fff; text-align: left; padding: 8px; border-bottom: 1px solid #555; z-index: 10; cursor: pointer; user-select: none; }
        th:hover { background: #444; }
        th.sort-asc { color: #00ffff; } th.sort-desc { color: #ff00ff; }
        th:nth-child(1) { width: 30%; } th:nth-child(2) { width: 15%; } th:nth-child(3) { width: 15%; } th:nth-child(4) { width: 25%; } th:nth-child(5) { width: 15%; }
        td { padding: 6px 8px; border-bottom: 1px solid #2a2a2a; cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-family: 'Courier New', monospace; }
        tr:hover { background: #333; }
        tr.active { background: #2c3e50; border-left: 4px solid #4da6ff; }
        
        .map-pane { flex-grow: 1; position: relative; background: #000; }
        #map, #skyplot { width: 100%; height: 100%; position: absolute; top: 0; left: 0; }
        #skyplot { display: none; background: #000; }
        .vis-yes { color: #0f0; font-weight: bold; }
        .vis-day { color: #ff0; }
        .vis-no { color: #0ff; }
        #curtain { position: absolute; top:0; left:0; width:100%; height:100%; background:black; z-index: 99999; display: flex; justify-content:center; align-items:center; color: white; }
        .view-toggle { margin-left: 10px; padding: 5px 10px; background: #333; border: 1px solid #555; color: #fff; cursor: pointer; font-family: monospace; }
        .view-toggle:hover { background: #444; }
        .house-icon { font-size: 24px; line-height: 24px; text-align: center; text-shadow: 2px 2px 4px #000; }
    </style>
</head>
<body>
    <div id="curtain"><h1>Loading v12.61...</h1></div>
    <div class="container">
        <div class="sidebar">
            <div class="header"><div><h2>VISIBLE EPHEMERIS</h2><div id="status">Connecting...</div></div><button class="view-toggle" onclick="toggleView()">MAP / SKY</button></div>
            <div class="table-wrap">
                <table>
                    <thead><tr><th onclick="sortBy('n')" id="th-n">Name</th><th onclick="sortBy('a')" id="th-a">Az</th><th onclick="sortBy('e')" id="th-e">El</th><th onclick="sortBy('next')" id="th-next">Next Event</th><th onclick="sortBy('v')" id="th-v">Vis</th></tr></thead>
                    <tbody id="sat-list"></tbody>
                </table>
            </div>
        </div>
        <div class="map-pane"><div id="map"></div><canvas id="skyplot"></canvas></div>
    </div>
    <script>
        var currentView='map'; var sortCol='e'; var sortAsc=false;
        var map = L.map('map', {zoomControl: false}).setView([0, 0], 2);
        L.control.zoom({position: 'topright'}).addTo(map);
        L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {maxZoom: 19, attribution: 'OpenStreetMap'}).addTo(map);
        var canvas = document.getElementById('skyplot'); var ctx = canvas.getContext('2d');
        function resizeCanvas() { canvas.width = canvas.parentElement.clientWidth; canvas.height = canvas.parentElement.clientHeight; }
        window.addEventListener('resize', resizeCanvas); resizeCanvas();
        function toggleView() { currentView=(currentView==='map')?'sky':'map'; document.getElementById('map').style.display=(currentView==='map')?'block':'none'; document.getElementById('skyplot').style.display=(currentView==='sky')?'block':'none'; render(); }
        function sortBy(col) { if(sortCol===col) sortAsc=!sortAsc; else { sortCol=col; sortAsc=true; } updateHeaders(); renderTable(); }
        function updateHeaders() { ['n','a','e','next','v'].forEach(c=>document.getElementById('th-'+c).className=''); document.getElementById('th-'+sortCol).className=sortAsc?'sort-asc':'sort-desc'; }
        function animate() { if(currentView==='sky') drawSkyplot(lastData); requestAnimationFrame(animate); }
        function drawSkyplot(data) {
            ctx.fillStyle='#000'; ctx.fillRect(0, 0, canvas.width, canvas.height);
            var cx=canvas.width/2; var cy=canvas.height/2; var r=Math.min(cx, cy)*0.9;
            ctx.strokeStyle='#00aa00'; ctx.lineWidth=2;
            ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2*Math.PI); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(cx-r, cy); ctx.lineTo(cx+r, cy); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(cx, cy-r); ctx.lineTo(cx, cy+r); ctx.stroke();
            ctx.fillStyle='#00ff00'; ctx.font='16px monospace'; ctx.fillText('N',cx-5,cy-r-10); ctx.fillText('E',cx+r+10,cy+5);
            data.forEach(s => {
                if(s.e<0) return;
                var dist=r*(90-s.e)/90.0; var rad=(s.a-90)*(Math.PI/180.0);
                var x=cx+dist*Math.cos(rad); var y=cy+dist*Math.sin(rad);
                if(s.id===selectedId) {
                    var t=Date.now(); var rad=8+4*Math.sin(t*0.005); var alp=0.3+0.2*Math.sin(t*0.005);
                    ctx.save(); ctx.beginPath(); ctx.arc(x, y, rad+5, 0, 2*Math.PI); ctx.fillStyle='rgba(255,0,255,'+alp+')'; ctx.fill(); ctx.strokeStyle='#ff00ff'; ctx.lineWidth=2; ctx.stroke(); ctx.restore();
                }
                ctx.fillStyle=(s.v==="YES")?"#0f0":(s.v==="DAY")?"#ff0":"#0ff";
                ctx.beginPath(); ctx.arc(x,y,5,0,2*Math.PI); ctx.fill();
                ctx.fillStyle='#fff'; ctx.font='12px monospace'; ctx.fillText(s.n,x+8,y+3);
            });
        }
        setTimeout(()=>document.getElementById('curtain').style.display='none', 1000);
        var markers={}; var polylines={}; var futureLines={}; var selectedId=null; var lastData=[]; 
        var houseMarker = null; var initialZoomDone = false;

        function selectSat(id, lat, lon) { selectedId=id; if(currentView==='map' && markers[id]) { map.panTo([lat, lon]); markers[id].openPopup(); } renderTable(); }
        function renderTable() {
            lastData.sort((a,b)=>{ var vA=a[sortCol], vB=b[sortCol]; if(typeof vA==='string'){vA=vA.toLowerCase();vB=vB.toLowerCase();} return (vA<vB)?(sortAsc?-1:1):(vA>vB)?(sortAsc?1:-1):0; });
            var html='';
            lastData.forEach(s=>{
                var vis=(s.v==="YES")?"vis-yes":(s.v==="DAY")?"vis-day":"vis-no"; var act=(s.id===selectedId)?"active":"";
                html+=`<tr class="${act}" onclick="selectSat(${s.id},${s.lat},${s.lon})"><td>${s.n}</td><td>${s.a.toFixed(1)}</td><td>${s.e.toFixed(1)}</td><td>${s.next}</td><td class="${vis}">${s.v}</td></tr>`;
            });
            document.getElementById('sat-list').innerHTML=html;
        }
        function renderMap(config) {
            if(currentView==='sky') return;
            if (!houseMarker && config) {
                houseMarker = L.marker([config.lat, config.lon], {
                    icon: L.divIcon({html: '🏠', className: 'house-icon', iconSize: [30,30], iconAnchor: [15,15]})
                }).addTo(map).bindPopup("Observer Location");
            }
            if (!initialZoomDone && config.max_apo > 0) {
                setTimeout(function() {
                    if (map.getSize().x > 0) {
                        var horizonKm = Math.sqrt(2 * 6378.0 * config.max_apo); 
                        var degRadius = (horizonKm / 111.0) * 1.22; 
                        map.invalidateSize();
                        map.fitBounds([[config.lat - degRadius, config.lon - degRadius],[config.lat + degRadius, config.lon + degRadius]]);
                        initialZoomDone = true;
                    } else { map.setView([config.lat, config.lon], 3); }
                }, 500); 
            } else if (!initialZoomDone) { map.setView([config.lat, config.lon], 3); }
            var currentIds=new Set();
            lastData.forEach(s=>{
                currentIds.add(s.id);
                var color=(s.v==="YES")?"#00ff00":(s.v==="DAY")?"#ffff00":"#00ffff";
                var hl=(s.id===selectedId)?"font-size:1.5em;font-weight:bold;color:#ff00ff;":"";
                var popup=`<div style="color:black"><b>${s.n}</b><br>Az: ${s.a.toFixed(1)}&deg;<br><span style="${hl}">El: ${s.e.toFixed(1)}&deg;</span><br>Next: ${s.next}</div>`;
                if(markers[s.id]) { markers[s.id].setLatLng([s.lat, s.lon]); if(markers[s.id].getPopup().isOpen()) markers[s.id].setPopupContent(popup); else markers[s.id].bindPopup(popup); markers[s.id].setStyle({color:color, fillColor:color}); }
                else { markers[s.id]=L.circleMarker([s.lat, s.lon], {color:color, radius:6, weight:1, fillColor:color, fillOpacity:0.9}).addTo(map).bindPopup(popup).on('click', ()=>selectSat(s.id, s.lat, s.lon)); }
                if(s.trail) { if(polylines[s.id]) polylines[s.id].setLatLngs(s.trail); else polylines[s.id]=L.polyline(s.trail, {color:'#0ff', weight:2, opacity:0.7, dashArray: '5,5'}).addTo(map); }
            });
            for(var id in markers) if(!currentIds.has(parseInt(id))) { map.removeLayer(markers[id]); delete markers[id]; }
            for(var id in polylines) if(!currentIds.has(parseInt(id))) { map.removeLayer(polylines[id]); delete polylines[id]; }
        }
        function updateSats() { fetch('/api/satellites?t='+Date.now()).then(r=>r.json()).then(d=>{ document.getElementById('status').innerText="Live: "+d.satellites.length; lastData=d.satellites; renderMap(d.config); renderTable(); }).catch(e=>console.error(e)); }
        setInterval(updateSats, 1000); updateSats(); requestAnimationFrame(animate);
    </script>
</body>
</html>
)HTML";

    WebServer::WebServer(int port) : port_(port), server_fd_(-1), running_(false) {
        // BIND IN CONSTRUCTOR TO FAIL FAST
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) throw std::runtime_error("WebServer: Failed to create socket");
        int opt = 1; setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in address; address.sin_family = AF_INET; address.sin_addr.s_addr = INADDR_ANY; address.sin_port = htons(port_);
        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("WebServer: Failed to bind port " + std::to_string(port_));
        }
        if (listen(server_fd_, 10) < 0) throw std::runtime_error("WebServer: Failed to listen");
        std::cout << "[INFO] WebServer started on port " << port_ << std::endl;
    }

    WebServer::~WebServer() { stop(); }

    void WebServer::start() {
        running_ = true;
        server_thread_ = std::thread(&WebServer::serverLoop, this);
    }

    void WebServer::stop() {
        running_ = false;
        if (server_fd_ >= 0) { shutdown(server_fd_, SHUT_RDWR); close(server_fd_); server_fd_ = -1; }
        if(server_thread_.joinable()) server_thread_.join();
    }

    void WebServer::updateData(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_json_data_ = buildJson(rows, raw_sats, config);
    }

    std::string WebServer::buildJson(const std::vector<DisplayRow>& rows, const std::vector<Satellite*>& raw_sats, const AppConfig& config) {
        std::stringstream ss;
        
        // PROTOCOL UPGRADE: WRAP SATELLITES IN OBJECT WITH META CONFIG
        ss << "{\"config\":{\"lat\":" << config.lat << ",\"lon\":" << config.lon << ",\"max_apo\":" << config.max_apo << "},";
        
        ss << "\"satellites\":[";
        for(size_t i=0; i<rows.size(); ++i) {
            const auto& r = rows[i];
            ss << "{\"id\":" << r.norad_id << ",\"n\":\"" << r.name << "\",\"lat\":" << r.lat << ",\"lon\":" << r.lon 
               << ",\"a\":" << r.az << ",\"e\":" << r.el << ",\"r\":" << r.range << ",\"apo\":" << r.apogee 
               << ",\"v\":\"" << (r.state==VisibilityCalculator::State::VISIBLE?"YES": (r.state==VisibilityCalculator::State::DAYLIGHT?"DAY":"NO")) 
               << "\",\"next\":\"" << r.next_event << "\"";
            
            auto it = std::find_if(raw_sats.begin(), raw_sats.end(), [&](Satellite* s){ return s->getNoradId() == r.norad_id; });
            if (it != raw_sats.end()) {
                // FIX: Use unified track API
                const auto& trail = (*it)->getFullTrackCopy();
                if(!trail.empty()) {
                    ss << ",\"trail\":[";
                    for(size_t j=0; j<trail.size(); ++j) {
                        ss << "[" << trail[j].lat_deg << "," << trail[j].lon_deg << "]";
                        if(j < trail.size()-1) ss << ",";
                    }
                    ss << "]";
                }
            }
            ss << "}";
            if(i < rows.size()-1) ss << ",";
        }
        ss << "]}"; // Close satellites array and main object
        return ss.str();
    }

    void WebServer::serverLoop() {
        while (running_) {
            int new_socket = accept(server_fd_, NULL, NULL);
            if (new_socket >= 0) {
                struct timeval t_out; t_out.tv_sec = 2; t_out.tv_usec = 0;
                setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t_out, sizeof(t_out));
                char buffer[2048]; int bytes = read(new_socket, buffer, 2048);
                if (bytes > 0) {
                    std::string request(buffer); std::string response;
                    std::string headers = "Connection: close\r\nCache-Control: no-cache\r\n";
                    if (request.find("GET /api/satellites") != std::string::npos) {
                        std::lock_guard<std::mutex> lock(data_mutex_);
                        std::string content = current_json_data_;
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" + headers + "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n" + content;
                    } else if (request.find("GET / ") != std::string::npos || request.find("GET /index.html") != std::string::npos) {
                        std::string content = HTML_CONTENT;
                        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n" + headers + "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n" + content;
                    } else {
                        std::string content = "404 Not Found"; response = "HTTP/1.1 404 Not Found\r\n" + headers + "\r\n" + content;
                    }
                    send(new_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
                }
                close(new_socket);
            } else { if (!running_) break; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }
    }
}
