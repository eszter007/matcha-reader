#pragma once

// Optional extra web routes.
//
// CrossPointWebServer calls registerRoutes() once, at the end of begin(), so a
// build can attach handlers of its own to the running server without the server
// knowing what they are.
//
// This build registers nothing: the default implementation in
// src/network/WebExtra.cpp is empty, so the route table is exactly the one
// CrossPointWebServer sets up itself and any unknown path falls through to the
// 404 handler as before.

class WebServer;

namespace webextra {

// Called with the live server after all built-in routes are registered and
// before onNotFound. Never null.
void registerRoutes(WebServer* server);

}  // namespace webextra
