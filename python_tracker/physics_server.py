"""TCP physics-stream server for the Python tracker.

Broadcasts marker-delimited full-state frames to connected clients on port 12346,
buffering data only while clients are attached. Functional twin of the C++
PhysicsServer (src/physics_server.cpp).
"""
import socket
import threading
import time

class PhysicsServer:
    """TCP server that broadcasts tracking frames to connected clients (port 12346)."""

    def __init__(self, port=12346):
        self.port = port
        self.server_socket = None
        self.running = False
        self.current_data = ""
        self.data_updated = False
        self.clients = set()
        self.data_lock = threading.Lock()
        self.clients_lock = threading.Lock()

        try:
            self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_socket.bind(('0.0.0.0', self.port))
            self.server_socket.listen(10)
            self.server_socket.setblocking(False)
            print(f"PhysicsServer started on port {self.port}")
        except Exception as e:
            print(f"Failed to start PhysicsServer on port {self.port}: {e}")
            self.server_socket = None

    def start(self):
        if not self.server_socket:
            return
        self.running = True
        self.accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.broadcast_thread = threading.Thread(target=self._broadcast_loop, daemon=True)
        self.accept_thread.start()
        self.broadcast_thread.start()

    def stop(self):
        self.running = False
        # Close all client connections
        with self.clients_lock:
            for client in list(self.clients):
                try:
                    client.close()
                except OSError:
                    pass
            self.clients.clear()

        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass

    def update_data(self, text):
        with self.data_lock:
            self.current_data = text
            self.data_updated = True

    def has_clients(self):
        with self.clients_lock:
            return len(self.clients) > 0

    def _accept_loop(self):
        while self.running:
            try:
                client_sock, addr = self.server_socket.accept()
                client_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                client_sock.setblocking(False)
                with self.clients_lock:
                    self.clients.add(client_sock)
                print(f"Physics client connected from {addr[0]}")
            except BlockingIOError:
                time.sleep(0.05)
            except OSError:
                if not self.running:
                    break

    def _broadcast_loop(self):
        while self.running:
            data_to_send = None

            with self.data_lock:
                if self.data_updated and self.current_data:
                    data_to_send = self.current_data + "\n---END_FRAME---\n"
                    self.data_updated = False

            if data_to_send and self.has_clients():
                dead_clients = []
                with self.clients_lock:
                    for client in list(self.clients):
                        try:
                            client.sendall(data_to_send.encode('utf-8'))
                        except (BrokenPipeError, ConnectionResetError, OSError):
                            dead_clients.append(client)

                # Remove disconnected clients
                for client in dead_clients:
                    with self.clients_lock:
                        self.clients.discard(client)
                    try:
                        client.close()
                    except OSError:
                        pass
                    print("Physics client disconnected")

            time.sleep(0.01)
