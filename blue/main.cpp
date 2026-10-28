// Blue team service: POST /inspect {"prompt": "..."} -> {"allow":bool,"category":..,"reason":..}
#include <cstdlib>
#include <iostream>

#include "json.hpp"
#include "net.hpp"
#include "rules.hpp"

using namespace bg;

int main() {
    const char* p = std::getenv("PORT");
    int port = p ? std::atoi(p) : 8001;
    std::cout << "[blue] listening on :" << port << std::endl;
    serve(port, [](const Request& rq) -> Response {
        Response rs;
        if (rq.method == "GET" && rq.path == "/health") { rs.body = "{\"status\":\"ok\"}"; return rs; }
        if (rq.method == "POST" && rq.path == "/inspect") {
            JObj o;
            if (!json_parse_flat(rq.body, o) || !o.count("prompt")) {
                rs.status = 400;
                rs.body = "{\"error\":\"expected {\\\"prompt\\\": \\\"...\\\"}\"}";
                return rs;
            }
            Verdict v = inspect(o["prompt"].v);
            rs.body = std::string("{\"allow\":") + (v.allow ? "true" : "false") + ",\"category\":" +
                      (v.category.empty() ? "null" : "\"" + json_escape(v.category) + "\"") + ",\"reason\":\"" +
                      json_escape(v.reason) + "\"}";
            return rs;
        }
        rs.status = 404;
        rs.body = "{\"error\":\"not found\"}";
        return rs;
    });
}
