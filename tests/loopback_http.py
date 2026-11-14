"""Local test HTTP peers must not depend on reverse DNS availability."""

import http.server
import socketserver


class LoopbackHTTPServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = str(self.server_address[0])
        self.server_port = self.server_address[1]
