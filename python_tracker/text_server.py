import socket
import threading
import time

class TextServer:
    def __init__(self, port=12345):
        self.port = port
        self.server_socket = None
        self.running = False
        self.current_text = "Waiting for data..."
        self.lock = threading.Lock()

        try:
            self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_socket.bind(('0.0.0.0', self.port))
            self.server_socket.listen(5)
            print(f"TextServer started on port {self.port}")
        except Exception as e:
            print(f"Failed to start TextServer on port {self.port}: {e}")
            self.server_socket = None

    def start(self):
        if not self.server_socket:
            return
        self.running = True
        self.thread = threading.Thread(target=self._server_loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except:
                pass

    def update_data(self, text):
        with self.lock:
            self.current_text = text

    def _server_loop(self):
        while self.running:
            try:
                # Accept with timeout to allow checking self.running
                self.server_socket.settimeout(1.0)
                try:
                    client_sock, addr = self.server_socket.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break # Socket closed

                # Handle client in a simple blocking manner (Fire-and-Forget)
                try:
                    client_sock.settimeout(0.5)
                    # Drain request
                    try:
                        client_sock.recv(4096)
                    except:
                        pass # Ignore read errors, we just want to send

                    with self.lock:
                        body_content = self.current_text

                    # HTML Wrap
                    html = (
                        "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='1'>"
                        "<title>Visible Ephemeris Terminal</title>"
                        "<style>body { background: #000; color: #0f0; font-family: monospace; font-size: 14px; white-space: pre; }</style>"
                        "</head><body>" + body_content + "</body></html>"
                    )

                    response = (
                        "HTTP/1.0 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        f"Content-Length: {len(html.encode('utf-8'))}\r\n"
                        "Connection: close\r\n\r\n" + html
                    )

                    client_sock.sendall(response.encode('utf-8'))
                except Exception as e:
                    # print(f"TextServer Error handling client: {e}")
                    pass
                finally:
                    client_sock.close()

            except Exception as e:
                print(f"TextServer Loop Error: {e}")
                if not self.server_socket: break
