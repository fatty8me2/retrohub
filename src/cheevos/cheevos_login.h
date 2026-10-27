#pragma once
/* One-time RetroAchievements sign-in (Settings > Account). The password goes to RA's server over HTTPS exactly once and is
 * never stored or logged; only the returned token is kept (cheevos::Config). */
#include <functional>
#include <string>

#include "cheevos_data.h"

namespace cheevos {

/* POST url (content type, body) -> response body + HTTP status. Returns false on a transport failure. */
using HttpPost = std::function<bool(const std::string& url, const std::string& content_type, const std::string& post,
                                    std::string* body, int* status)>;
/* The real one: gameinfo_net::http_post (WinHTTP). */
HttpPost real_http();

/* On success fills cfg->username (case-corrected) + cfg->token, leaves enabled/hardcore untouched. */
bool login_with_password(const std::string& user, const std::string& password, const HttpPost& http, Config* cfg, std::string* err);

}  // namespace cheevos
