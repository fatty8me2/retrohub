#include "cheevos_login.h"

#include "gameinfo_net.h"
#include "rc_api_user.h"

namespace cheevos {

HttpPost real_http() {
    return [](const std::string& url, const std::string& ct, const std::string& post, std::string* body, int* status) {
        return gameinfo_net::http_post(url, ct, post, body, status);
    };
}

bool login_with_password(const std::string& user, const std::string& password, const HttpPost& http, Config* cfg, std::string* err) {
    if (user.empty() || password.empty()) { *err = "Enter your RetroAchievements username and password."; return false; }
    rc_api_login_request_t p{};
    p.username = user.c_str();
    p.password = password.c_str();
    rc_api_request_t req;
    if (rc_api_init_login_request(&req, &p) != 0) { *err = "Could not build the login request."; return false; }
    std::string body;
    int status = 0;
    bool sent = http(req.url, req.content_type ? req.content_type : "application/x-www-form-urlencoded", req.post_data ? req.post_data : "", &body, &status);
    rc_api_destroy_request(&req);
    if (!sent || status == 0) { *err = "Can't reach RetroAchievements. Check your internet connection."; return false; }
    rc_api_login_response_t r{};
    rc_api_server_response_t sr{};
    sr.body = body.c_str();
    sr.body_length = body.size();
    sr.http_status_code = status;
    int rc = rc_api_process_login_server_response(&r, &sr);
    bool ok = rc == 0 && r.response.succeeded && r.api_token && *r.api_token;
    if (ok) {
        cfg->username = r.username && *r.username ? r.username : user;
        cfg->token = r.api_token;
    } else {
        *err = r.response.error_message && *r.response.error_message ? r.response.error_message : "RetroAchievements refused the sign-in.";
    }
    rc_api_destroy_login_response(&r);
    return ok;
}

}  // namespace cheevos
