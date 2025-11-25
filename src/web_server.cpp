#include "web_server.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h> 

namespace ve {

    // Safe HTML Client (No backticks in C++ string literal)
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
        .sidebar { width: 30%; min-width: 320px; background: #1a1a1a; border-right: 1px solid #333; display: flex; flex-direction: column; }
        .header { padding: 15px; background: #222; border-bottom: 1px solid #444; display:flex; justify-content:space-between; align-items:center;}
        .header h2 { margin: 0; color: #4da6ff; font-size: 18px; }
        #status { font-size: 11px; color: #888; margin-top: 5px; }
        .table-wrap { flex-grow: 1; overflow-y: auto; }
        table { width: 100%; border-collapse: collapse; font-size: 12px; }
        th { position: sticky; top: 0; background: #333; color: #fff; text-align: left; padding: 8px; border-bottom: 1px solid #555; z-index: 10; }
        td { padding: 6px 8px; border-bottom: 1px solid #2a2a2a; cursor: pointer; }
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
    </style>
</head>
<body>
    <div id="curtain"><h1>Loading v12.0...</h1></div>
    
    <div class="container">
        <div class="sidebar">
            <div class="header">
                <div>
                    <h2>VISIBLE EPHEMERIS</h2>
                    <div id="status">Connecting...</div>
                </div>
                <button class="view-toggle" onclick="toggleView()">MAP / SKY</button>
            </div>
            <div class="table-wrap">
                <table>
                    <thead><tr><th>Name</th><th>Az</th><th>El</th><th>Apogee</th><th>Vis</th></tr></thead>
                    <tbody id="sat-list"></tbody>
                </table>
            </div>
        </div>
        <div class="map-pane">
            <div id="map"></div>
            <canvas id="skyplot"></canvas>
        </div>
    </div>

    <script>
        var currentView = 'map'; 

        var map = L.map('map', {zoomControl: false}).setView([0, 0], 2);
        L.control.zoom({position: 'topright'}).addTo(map);
        L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
            maxZoom: 19,
            attribution: 'OpenStreetMap'
        }).addTo(map);
        
        var canvas = document.getElementById('skyplot');
        var ctx = canvas.getContext('2d');
        
        function resizeCanvas() {
            canvas.width = canvas.parentElement.clientWidth;
            canvas.height = canvas.parentElement.clientHeight;
        }
        window.addEventListener('resize', resizeCanvas);
        resizeCanvas();

        function toggleView() {
            if (currentView === 'map') {
                currentView = 'sky';
                document.getElementById('map').style.display = 'none';
                document.getElementById('skyplot').style.display = 'block';
            } else {
                currentView = 'map';
                document.getElementById('map').style.display = 'block';
                document.getElementById('skyplot').style.display = 'none';
            }
            render();
        }

        function drawSkyplot(data) {
            ctx.fillStyle = '#000';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
            
            var cx = canvas.width / 2;
            var cy = canvas.height / 2;
            var r = Math.min(cx, cy) * 0.9;
            
            // Grid
            ctx.strokeStyle = '#004400';
            ctx.lineWidth = 1;
            ctx.beginPath(); ctx.arc(cx, cy, r, 0, 2*Math.PI); ctx.stroke();
            ctx.beginPath(); ctx.arc(cx, cy, r*0.66, 0, 2*Math.PI); ctx.stroke();
            ctx.beginPath(); ctx.arc(cx, cy, r*0.33, 0, 2*Math.PI); ctx.stroke();
            
            // Crosshairs
            ctx.beginPath(); ctx.moveTo(cx-r, cy); ctx.lineTo(cx+r, cy); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(cx, cy-r); ctx.lineTo(cx, cy+r); ctx.stroke();
            
            // Text
            ctx.fillStyle = '#008800';
            ctx.font = '12px monospace';
            ctx.fillText('N', cx-4, cy-r-5);
            ctx.fillText('S', cx-4, cy+r+12);
            ctx.fillText('E', cx+r+5, cy+4);
            ctx.fillText('W', cx-r-15, cy+4);
            
            // Satellites
            data.forEach(function(s) {
                var el = s.e;
                if (el < 0) return; 
                
                var dist = r * (90 - el) / 90.0;
                var rad = (s.a - 90) * (Math.PI / 180.0); 
                
                var x = cx + dist * Math.cos(rad);
                var y = cy + dist * Math.sin(rad);
                
                var color = (s.v === "YES") ? "#0f0" : (s.v === "DAY") ? "#ff0" : "#0ff";
                
                ctx.fillStyle = color;
                ctx.beginPath();
                ctx.arc(x, y, 4, 0, 2*Math.PI);
                ctx.fill();
                
                ctx.fillStyle = '#fff';
                ctx.fillText(s.n, x+6, y+3);
            });
        }

        setTimeout(function(){ document.getElementById('curtain').style.display = 'none'; }, 1000);

        var markers = {}; 
        var selectedId = null;
        var lastData = [];

        function selectSat(id, lat, lon) {
            selectedId = id;
            if (currentView === 'map' && markers[id]) {
                map.panTo([lat, lon]);
                markers[id].openPopup();
            }
            renderTable();
        }
        
        function renderTable() {
            var tbody = document.getElementById('sat-list');
            var html = '';
            lastData.forEach(function(s) {
                var visClass = (s.v === "YES") ? "vis-yes" : (s.v === "DAY") ? "vis-day" : "vis-no";
                var activeClass = (s.id === selectedId) ? "active" : "";
                
                html += '<tr class="' + activeClass + '" onclick="selectSat(' + s.id + ',' + s.lat + ',' + s.lon + ')">' +
                    '<td>' + s.n + '</td>' +
                    '<td>' + s.a.toFixed(1) + '</td>' +
                    '<td>' + s.e.toFixed(1) + '</td>' +
                    '<td>' + s.apo.toFixed(0) + '</td>' +
                    '<td class="' + visClass + '">' + s.v + '</td>' +
                '</tr>';
            });
            tbody.innerHTML = html;
        }
        
        function render() {
            if (currentView === 'sky') {
                drawSkyplot(lastData);
            } else {
                // Update Leaflet Markers
                 var currentIds = new Set();
                 lastData.forEach(function(s) {
                    currentIds.add(s.id);
                    var color = (s.v === "YES") ? "#00ff00" : (s.v === "DAY") ? "#ffff00" : "#00ffff";
                    
                    // Safe string concat for popup (No backticks)
                    var popupText = '<div style="color:black"><b>' + s.n + '</b><br>Az: ' + s.a.toFixed(1) + '&deg; El: ' + s.e.toFixed(1) + '&deg;<br>Apogee: ' + s.apo.toFixed(0) + ' km</div>';

                    if (markers[s.id]) {
                        markers[s.id].setLatLng([s.lat, s.lon]);
                        markers[s.id].setPopupContent(popupText);
                        markers[s.id].setStyle({color: color, fillColor: color});
                    } else {
                        markers[s.id] = L.circleMarker([s.lat, s.lon], {
                            color: color,
                            radius: 6, weight: 1, fillColor: color, fillOpacity: 0.9
                        }).addTo(map).bindPopup(popupText).on('click', function() { selectSat(s.id, s.lat, s.lon); });
                    }
                });
                for (var id in markers) {
                    if (!currentIds.has(parseInt(id))) {
                        map.removeLayer(markers[id]);
                        delete markers[id];
                    }
                }
            }
        }

        function updateSats() {
            fetch('/api/satellites?t=' + Date.now(), { cache: "no-store" })
                .then(function(response) {
                    if (!response.ok) throw new Error("HTTP " + response.status);
                    return response.json();
                })
                .then(function(data) {
                    document.getElementById('status').innerText = "Live: " + data.length + " objects";
                    lastData = data;
                    render();
                    renderTable();
                })
                .catch(function(err) {
                    console.error("Fetch Error:", err);
                    document.getElementById('status').innerText = "Error: " + err.message;
                });
        }

        setInterval(updateSats, 1000); 
        updateSats();
    </script>
</body>
</html>
)HTML";

    WebServer::WebServer(int port) : port_(port), server_fd_(-1), running_(false) {}

    WebServer::~WebServer() { stop(); }

    void WebServer::start() {
        running_ = true;
        server_thread_ = std::thread(&WebServer::serverLoop, this);
    }

    void WebServer::stop() {
        running_ = false;
        if (server_fd_ >= 0) {
            shutdown(server_fd_, SHUT_RDWR);
            close(server_fd_);
            server_fd_ = -1;
        }
        if(server_thread_.joinable()) server_thread_.join();
    }

    void WebServer::updateData(const std::vector<DisplayRow>& rows) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_json_data_ = buildJson(rows);
    }

    std::string WebServer::buildJson(const std::vector<DisplayRow>& rows) {
        std::stringstream ss;
        ss << "[";
        for(size_t i=0; i<rows.size(); ++i) {
            const auto& r = rows[i];
            ss << "{\"id\":" << r.norad_id << ",\"n\":\"" << r.name << "\",\"lat\":" << r.lat << ",\"lon\":" << r.lon 
               << ",\"a\":" << r.az << ",\"e\":" << r.el << ",\"r\":" << r.range << ",\"apo\":" << r.apogee 
               << ",\"v\":\"" << (r.state==VisibilityCalculator::State::VISIBLE?"YES": (r.state==VisibilityCalculator::State::DAYLIGHT?"DAY":"NO")) << "\"}";
            if(i < rows.size()-1) ss << ",";
        }
        ss << "]";
        return ss.str();
    }

    void WebServer::serverLoop() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return;

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port_);

        if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) return;
        if (listen(server_fd_, 10) < 0) return;

        while (running_) {
            int new_socket = accept(server_fd_, NULL, NULL);
            if (new_socket >= 0) {
                struct timeval t_out; t_out.tv_sec = 2; t_out.tv_usec = 0;
                setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t_out, sizeof(t_out));

                char buffer[2048];
                memset(buffer, 0, 2048);
                int bytes = read(new_socket, buffer, 2048);
                
                if (bytes > 0) {
                    std::string request(buffer);
                    std::string response;
                    std::string first_line = request.substr(0, request.find("\r\n"));
                    
                    std::string headers = "Connection: close\r\n"
                                          "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                                          "Pragma: no-cache\r\n"
                                          "Expires: 0\r\n";

                    if (request.find("GET /api/satellites") != std::string::npos) {
                        std::lock_guard<std::mutex> lock(data_mutex_);
                        std::string content = current_json_data_;
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" + headers + 
                                   "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n" + content;
                    } 
                    else if (request.find("GET / ") != std::string::npos || request.find("GET /index.html") != std::string::npos) {
                        std::string content = HTML_CONTENT;
                        response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n" + headers +
                                   "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n" + content;
                    }
                    else {
                        std::string content = "404 Not Found";
                        response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n" + headers +
                                   "Content-Length: " + std::to_string(content.length()) + "\r\n\r\n" + content;
                    }
                    
                    send(new_socket, response.c_str(), response.length(), MSG_NOSIGNAL);
                }
                close(new_socket);
            } else {
                if (!running_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        close(server_fd_);
    }
}
