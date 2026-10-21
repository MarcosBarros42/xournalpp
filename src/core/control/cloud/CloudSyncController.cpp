/*
 * Xournal++
 *
 * Google Drive cloud sync controller (fork feature)
 *
 * @license GNU GPLv2 or later
 */

#include "CloudSyncController.h"

#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#ifdef DELETE
#undef DELETE
#endif

#include "control/Control.h"
#include "control/cloud/CloudJobs.h"
#include "control/jobs/XournalScheduler.h"
#include "control/settings/Settings.h"
#include "model/Document.h"
#include "util/PathUtil.h"
#include "util/Util.h"
#include "util/i18n.h"

#ifdef _WIN32
#define CLOUD_CLOSE_SOCKET(s) closesocket(s)
#else
#define CLOUD_CLOSE_SOCKET(s) close(s)
#endif

constexpr auto OAUTH_AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr auto OAUTH_TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr auto DRIVE_API_URL = "https://www.googleapis.com/drive/v3/files";
constexpr auto DRIVE_UPLOAD_URL = "https://www.googleapis.com/upload/drive/v3/files";
constexpr auto DRIVE_SCOPE = "https://www.googleapis.com/auth/drive.file";
constexpr auto DRIVE_FOLDER_NAME = "Xournal++";
constexpr auto MULTIPART_BOUNDARY = "xojsync_21ae4d098b01d5";

namespace {
    struct CurlWriteCtx {
        std::string data;
    };

    auto writeToString(void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        auto* ctx = static_cast<CurlWriteCtx*>(userp);
        ctx->data.append(static_cast<const char*>(contents), size * nmemb);
        return size * nmemb;
    }

    struct CurlGlobalInit {
        CurlGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobalInit() { curl_global_cleanup(); }
        CurlGlobalInit(const CurlGlobalInit&) = delete;
        auto operator=(const CurlGlobalInit&) -> CurlGlobalInit& = delete;
    };

    struct CurlState {
        CurlState() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlState() { curl_global_cleanup(); }
        CurlState(const CurlState&) = delete;
        auto operator=(const CurlState&) -> CurlState& = delete;
    };
    auto curlState() -> CurlState& {
        static CurlState state;
        return state;
    }

    auto urlEncode(const std::string& raw) -> std::string {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return raw;
        }
        std::string result;
        if (char* escaped = curl_easy_escape(curl, raw.c_str(), static_cast<int>(raw.length()))) {
            result = escaped;
            curl_free(escaped);
        }
        curl_easy_cleanup(curl);
        return result;
    }

    struct JsonDoc {
        JsonParser* parser = nullptr;
        JsonObject* obj = nullptr;
        JsonArray* arr = nullptr;
        ~JsonDoc() {
            if (parser) {
                g_object_unref(parser);
            }
        }
    };

    auto jsonOpenDoc(const std::string& data, JsonDoc& doc) -> std::string {
        doc.parser = json_parser_new();
        GError* err = nullptr;
        if (!json_parser_load_from_data(doc.parser, data.c_str(), static_cast<gssize>(data.size()), &err)) {
            std::string msg = err && err->message ? err->message : "invalid JSON";
            g_error_free(err);
            g_object_unref(doc.parser);
            doc.parser = nullptr;
            return msg;
        }
        JsonNode* root = json_parser_get_root(doc.parser);
        if (!root) {
            return "empty JSON";
        }
        if (json_node_get_value_type(root) == JSON_TYPE_OBJECT) {
            doc.obj = json_node_get_object(root);
        } else if (json_node_get_value_type(root) == JSON_TYPE_ARRAY) {
            doc.arr = json_node_get_array(root);
        }
        return {};
    }

    auto jsonGetString(JsonObject* obj, const char* name) -> std::string {
        if (!obj) {
            return {};
        }
        JsonNode* node = json_object_get_member(obj, name);
        if (!node || json_node_get_value_type(node) != G_TYPE_STRING) {
            return {};
        }
        if (const char* s = json_node_get_string(node); s != nullptr) {
            return s;
        }
        return {};
    }

    auto jsonGetInt(JsonObject* obj, const char* name) -> int {
        if (!obj) {
            return 0;
        }
        JsonNode* node = json_object_get_member(obj, name);
        if (!node || json_node_get_value_type(node) != G_TYPE_INT64) {
            return 0;
        }
        return static_cast<int>(json_node_get_int(node));
    }

    auto escapeJsonString(const std::string& raw) -> std::string {
        std::string out;
        out.reserve(raw.size() + 8);
        for (char c: raw) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        g_snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

#ifdef _WIN32
    auto readAuthCodeFromRequest(const std::string& request, std::string& codeOut, std::string& errOut) -> bool {
        size_t qmark = request.find('?');
        if (qmark == std::string::npos) {
            errOut = "no authorization code received";
            return false;
        }
        size_t sp = request.find(' ', qmark);
        if (sp == std::string::npos) {
            sp = request.size();
        }
        std::string query = request.substr(qmark + 1, sp - qmark - 1);
        size_t codePos = query.find("code=");
        if (codePos != std::string::npos && (codePos == 0 || query[codePos - 1] == '&' || query[codePos - 1] == '?')) {
            codeOut = query.substr(codePos + 5);
            size_t amp = codeOut.find('&');
            if (amp != std::string::npos) {
                codeOut = codeOut.substr(0, amp);
            }
            return true;
        }
        size_t errPos = query.find("error=");
        if (errPos != std::string::npos &&
            (errPos == 0 || query[errPos - 1] == '&' || query[errPos - 1] == '?')) {
            std::string errVal = query.substr(errPos + 6);
            size_t amp = errVal.find('&');
            if (amp != std::string::npos) {
                errVal = errVal.substr(0, amp);
            }
            errOut = "authorization was denied: " + errVal;
        } else {
            errOut = "no authorization code received";
        }
        return false;
    }
#endif
}  // namespace

CloudSyncController::CloudSyncController(Control* controlIn):
        control(controlIn), credentialsFile(Util::getConfigFile("cloudclient.json")) {
    this->settings = this->control->getSettings();
    curlState();

    auto cloud = this->settings->getCustomElement("cloud");
    std::string refresh;
    std::string access;
    int expire = 0;
    cloud.getString("refreshToken", refresh);
    cloud.getString("accessToken", access);
    cloud.getInt("accessExpire", expire);

    const std::lock_guard<std::mutex> lock{tokenMtx};
    this->refreshToken = std::move(refresh);
    this->accessToken = std::move(access);
    this->accessExpireEpoch = expire;
}

CloudSyncController::~CloudSyncController() { clearStatusCallback(); }

auto CloudSyncController::isConfigured() const -> bool {
    return fs::exists(this->credentialsFile);
}

auto CloudSyncController::isLoggedIn() const -> bool {
    const std::lock_guard<std::mutex> lock{tokenMtx};
    return !this->refreshToken.empty();
}

auto CloudSyncController::isLoginRunning() const -> bool {
    return this->loginRunning.load();
}

void CloudSyncController::setStatusCallback(std::function<void(const std::string&)> cb) {
    const std::lock_guard<std::mutex> lock{statusCbMtx};
    this->statusCallback = std::move(cb);
}

void CloudSyncController::clearStatusCallback() {
    const std::lock_guard<std::mutex> lock{statusCbMtx};
    this->statusCallback = nullptr;
}

void CloudSyncController::setStatus(const std::string& status) {
    Util::execInUiThread([status, this]() {
        const std::lock_guard<std::mutex> lock{statusCbMtx};
        if (this->statusCallback) {
            this->statusCallback(status);
        }
    });
}

auto CloudSyncController::getCredentialsPath() const -> std::string {
    return this->credentialsFile.string();
}

void CloudSyncController::login() {
    if (this->isLoginRunning()) {
        return;
    }
    if (this->isLoggedIn()) {
        this->setStatus(_("Google Drive: already connected."));
        return;
    }
    if (!this->isConfigured()) {
        this->setStatus(FS(_F("Google Drive: credentials file not found: {1}") % this->credentialsFile.string()));
        return;
    }
    this->loginRunning.store(true);
    auto* job = new CloudLoginJob(this);
    this->control->getScheduler()->addJob(job, JOB_PRIORITY_NONE);
    job->unref();
}

void CloudSyncController::logout() {
    this->setCloudTokens("", "", 0);
    this->setStatus(_("Google Drive: disconnected."));
}

void CloudSyncController::uploadDocument(const fs::path& path) {
    if (!this->isLoggedIn() || path.empty()) {
        return;
    }
    std::string folderId;
    std::string fileId;
    auto cloud = this->settings->getCustomElement("cloud");
    cloud.getString("folderId", folderId);
    if (!folderId.empty()) {
        auto ids = cloud.child("fileIds");
        ids.getString(path.filename().string(), fileId);
    }
    auto* job = new DriveUploadJob(this, path, std::move(folderId), std::move(fileId));
    this->control->getScheduler()->addJob(job, JOB_PRIORITY_NONE);
    job->unref();
}

void CloudSyncController::syncNow() {
    Document* doc = this->control->getDocument();
    doc->lock_shared();
    fs::path path = doc->getFilepath();
    doc->unlock_shared();
    if (path.empty()) {
        this->setStatus(_("Google Drive: save the document first."));
        return;
    }
    this->setStatus(FS(_F("Google Drive: syncing {1} ...") % path.filename().string()));
    this->uploadDocument(path);
}

void CloudSyncController::setCloudTokens(const std::string& accessTokenIn, const std::string& refreshTokenIn,
                                         long long expiresInSeconds) {
    {
        const std::lock_guard<std::mutex> lock{tokenMtx};
        this->accessToken = accessTokenIn;
        this->refreshToken = refreshTokenIn;
        this->accessExpireEpoch = accessTokenIn.empty() ? 0 : time(nullptr) + expiresInSeconds;
    }
    this->loginRunning.store(false);
    if (this->settings) {
        auto cloud = this->settings->getCustomElement("cloud");
        cloud.setString("refreshToken", this->refreshToken);
        if (this->accessToken.empty()) {
            cloud.setString("accessToken", "");
            cloud.setInt("accessExpire", 0);
        } else {
            cloud.setString("accessToken", this->accessToken);
            cloud.setInt("accessExpire", static_cast<int>(this->accessExpireEpoch));
        }
        this->settings->customSettingsChanged();
    }
}

void CloudSyncController::setCloudFileId(const fs::path& path, const std::string& fileId) {
    if (!this->settings) {
        return;
    }
    auto cloud = this->settings->getCustomElement("cloud");
    auto ids = cloud.child("fileIds");
    ids.setString(path.filename().string(), fileId);
    this->settings->customSettingsChanged();
}

void CloudSyncController::setCloudFolderId(const std::string& folderId) {
    if (!this->settings) {
        return;
    }
    auto cloud = this->settings->getCustomElement("cloud");
    cloud.setString("folderId", folderId);
    this->settings->customSettingsChanged();
}

auto CloudSyncController::loadCredentials(std::string& clientIdOut, std::string& clientSecretOut) const -> bool {
    std::ifstream in{this->credentialsFile, std::ios::binary};
    if (!in.good()) {
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    if (data.empty()) {
        return false;
    }
    JsonDoc doc;
    if (!jsonOpenDoc(data, doc).empty() || !doc.obj) {
        return false;
    }
    JsonObject* conf = nullptr;
    if (json_object_has_member(doc.obj, "installed")) {
        conf = json_object_get_object_member(doc.obj, "installed");
    }
    if (!conf) {
        if (json_object_has_member(doc.obj, "web")) {
            conf = json_object_get_object_member(doc.obj, "web");
        }
    }
    if (!conf) {
        conf = doc.obj;
    }
    std::string id = jsonGetString(conf, "client_id");
    std::string secret = jsonGetString(conf, "client_secret");
    if (id.empty() || secret.empty()) {
        return false;
    }
    clientIdOut = std::move(id);
    clientSecretOut = std::move(secret);
    return true;
}

auto CloudSyncController::tokenRequest(const std::string& body, std::string& accessTokenOut,
                                       std::string& refreshTokenOut, int& expiresInSecOut) -> std::string {
    CurlWriteCtx response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "curl init failed";
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_URL, OAUTH_TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    std::string result = res != CURLE_OK ? curl_easy_strerror(res) : std::string();
    if (result.empty()) {
        if (httpCode >= 400) {
            result = FS(_F("HTTP {1}: {2}") % httpCode % (response.data.empty() ? std::string("(empty)") : response.data));
        } else {
            JsonDoc doc;
            std::string err = jsonOpenDoc(response.data, doc);
            if (!err.empty()) {
                result = err;
            } else if (doc.obj) {
                std::string errMsg = jsonGetString(doc.obj, "error_description");
                if (errMsg.empty()) {
                    errMsg = jsonGetString(doc.obj, "error");
                }
                if (!errMsg.empty()) {
                    result = errMsg;
                } else {
                    std::string access = jsonGetString(doc.obj, "access_token");
                    if (access.empty()) {
                        result = "empty token response";
                    } else {
                        accessTokenOut = std::move(access);
                        refreshTokenOut = jsonGetString(doc.obj, "refresh_token");
                        expiresInSecOut = jsonGetInt(doc.obj, "expires_in");
                    }
                }
            }
        }
    }
    return result;
}

auto CloudSyncController::refreshAccessToken() -> std::string {
    std::string clientId;
    std::string secret;
    if (!this->loadCredentials(clientId, secret)) {
        return FS(_F("Google Drive: invalid credentials file: {1}") % this->credentialsFile.string());
    }
    std::string refresh;
    {
        const std::lock_guard<std::mutex> lock{tokenMtx};
        refresh = this->refreshToken;
    }
    if (refresh.empty()) {
        return "not logged in";
    }
    std::ostringstream body;
    body << "client_id=" << urlEncode(clientId) << "&client_secret=" << urlEncode(secret)
         << "&refresh_token=" << urlEncode(refresh) << "&grant_type=refresh_token";
    std::string access;
    std::string refreshOut;
    int expiry = 0;
    std::string result = this->tokenRequest(body.str(), access, refreshOut, expiry);
    if (!result.empty()) {
        return result;
    }
    const std::lock_guard<std::mutex> lock{tokenMtx};
    this->accessToken = std::move(access);
    this->accessExpireEpoch = time(nullptr) + expiry;
    return {};
}

auto CloudSyncController::ensureAccessToken() -> std::string {
    {
        const std::lock_guard<std::mutex> lock{tokenMtx};
        if (!this->accessToken.empty() && this->accessExpireEpoch - 60 > time(nullptr)) {
            return {};
        }
    }
    return this->refreshAccessToken();
}

auto CloudSyncController::httpCall(const std::string& method, const std::string& url, const std::string& body,
                                   const std::string& contentType, const std::string& bearerToken, long timeoutSec,
                                   std::string& responseOut) -> std::string {
    responseOut.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "curl init failed";
    }
    struct curl_slist* headers = nullptr;
    if (!bearerToken.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + bearerToken).c_str());
    }
    bool hasBody = method != "GET" && !body.empty();
    if (hasBody && !contentType.empty()) {
        headers = curl_slist_append(headers, ("Content-Type: " + contentType).c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#ifdef _WIN32
    std::wstring exeFile(MAX_PATH, L'\0');
    DWORD exeLen = GetModuleFileNameW(nullptr, exeFile.data(), static_cast<DWORD>(exeFile.size()));
    std::filesystem::path exePath(std::wstring(exeFile.data(), exeLen));
    static const std::string caPemFile = (exePath.parent_path() / "cacert.pem").string();
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPemFile.c_str());
#endif
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseOut);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (hasBody) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        return curl_easy_strerror(res);
    }
    if (httpCode >= 400) {
        return FS(_F("HTTP {1}: {2}") % httpCode % (responseOut.empty() ? std::string("(empty)") : responseOut));
    }
    return {};
}

auto CloudSyncController::ensureDriveFolderId(std::string& folderIdInOut) -> std::string {
    if (!folderIdInOut.empty()) {
        return {};
    }
    std::string access;
    {
        const std::lock_guard<std::mutex> lock{tokenMtx};
        access = this->accessToken;
    }
    std::string q = std::string("name='") + DRIVE_FOLDER_NAME +
                    "' and mimeType='application/vnd.google-apps.folder' and trashed=false";
    std::string url = std::string(DRIVE_API_URL) + "?q=" + urlEncode(q) + "&fields=files(id)&spaces=drive";
    std::string response;
    std::string result = this->httpCall("GET", url, "", "", access, 30, response);
    if (!result.empty()) {
        return result;
    }
    JsonDoc doc;
    result = jsonOpenDoc(response, doc);
    if (!result.empty()) {
        return result;
    }
    std::string foundId;
    if (doc.arr && json_array_get_length(doc.arr) > 0) {
        JsonNode* el = json_array_get_element(doc.arr, 0);
        if (el && json_node_get_value_type(el) == JSON_TYPE_OBJECT) {
            foundId = jsonGetString(json_node_get_object(el), "id");
        }
    }
    if (foundId.empty()) {
        std::string meta = "{\"name\":\"" + escapeJsonString(DRIVE_FOLDER_NAME) +
                           "\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
        std::string body = std::string("--") + MULTIPART_BOUNDARY +
                           "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" + meta + "\r\n--" +
                           MULTIPART_BOUNDARY + "--\r\n";
        std::string multipartType = std::string("multipart/related; boundary=\"") + MULTIPART_BOUNDARY + "\"";
        result = this->httpCall("POST", std::string(DRIVE_UPLOAD_URL) + "?uploadType=multipart&fields=id", body,
                                multipartType, access, 30, response);
        if (result.empty()) {
            JsonDoc doc2;
            result = jsonOpenDoc(response, doc2);
            if (result.empty()) {
                foundId = jsonGetString(doc2.obj, "id");
                if (foundId.empty()) {
                    result = "folder creation returned no id";
                }
            }
        }
    }
    if (result.empty()) {
        folderIdInOut = foundId;
    }
    return result;
}

auto CloudSyncController::performLogin(std::string& accessTokenOut, std::string& refreshTokenOut,
                                       long long& expiresInSecondsOut) -> std::string {
    std::string clientId;
    std::string secret;
    if (!this->loadCredentials(clientId, secret)) {
        return FS(_F("Google Drive: invalid credentials file: {1}") % this->credentialsFile.string());
    }

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return "WSAStartup failed";
    }
#endif

    SOCKET listener = INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = 0;

    int port = 0;
    std::string result;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        result = "socket() failed";
    } else {
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            result = "bind() failed";
        } else if (::listen(listener, 1) < 0) {
            result = "listen() failed";
        } else {
            sockaddr_in bound{};
            socklen_t boundLen = sizeof(bound);
            if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLen) >= 0) {
                port = static_cast<int>(ntohs(bound.sin_port));
            } else {
                result = "getsockname() failed";
            }
        }
    }

    if (result.empty()) {
        const std::string redirectUri = "http://127.0.0.1:" + std::to_string(port);
        std::ostringstream authUrl;
        authUrl << OAUTH_AUTH_URL << "?scope=" << urlEncode(DRIVE_SCOPE) << "&response_type=code"
                << "&access_type=offline&prompt=consent&include_granted_scopes=true&client_id="
                << urlEncode(clientId) << "&redirect_uri=" << urlEncode(redirectUri);

        GError* gerr = nullptr;
        g_app_info_launch_default_for_uri(authUrl.str().c_str(), nullptr, &gerr);
        if (gerr) {
            std::string openBrowserErr = gerr->message ? gerr->message : "unknown";
            g_error_free(gerr);
            this->setStatus(FS(_F("Google Drive: open this URL manually: {1} (error: {2})") % authUrl.str() %
                               openBrowserErr));
        } else {
            this->setStatus(_("Google Drive: browser opened, please authorize the access..."));
        }

        std::string authCode;
        bool gotCode = false;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            result = "accept() failed";
        } else {
            std::string request;
            char buf[4096];
            for (;;) {
                int n = recv(client, buf, sizeof(buf) - 1, 0);
                if (n <= 0) {
                    break;
                }
                buf[n] = '\0';
                request += buf;
                if (request.size() > 65536 || request.find("\r\n\r\n") != std::string::npos ||
                    request.find("\n\n") != std::string::npos) {
                    break;
                }
            }
            const std::string resp = "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
                                     "<html><body><h1>Autoriza&ccedil;&atilde;o conclu&iacute;da</h1>"
                                     "<p>Voc&ecirc; pode fechar esta p&aacute;gina e voltar ao Xournal++.</p>"
                                     "</body></html>";
            send(client, resp.c_str(), static_cast<int>(resp.size()), 0);
            shutdown(client, 2);
            CLOUD_CLOSE_SOCKET(client);
#ifdef _WIN32
            gotCode = readAuthCodeFromRequest(request, authCode, result);
#else
            gotCode = false;
            result = "not supported on this platform";
#endif
        }
        if (listener != INVALID_SOCKET) {
            CLOUD_CLOSE_SOCKET(listener);
        }
        if (gotCode) {
            this->setStatus(_("Google Drive: exchanging authorization code for tokens..."));
            std::ostringstream body;
            body << "client_id=" << urlEncode(clientId) << "&client_secret=" << urlEncode(secret)
                 << "&code=" << urlEncode(authCode) << "&redirect_uri=" << urlEncode(redirectUri)
                 << "&grant_type=authorization_code";
            std::string access;
            std::string refresh;
            int expiry = 0;
            result = this->tokenRequest(body.str(), access, refresh, expiry);
            if (result.empty() && refresh.empty()) {
                result = "no refresh token received";
            }
            if (result.empty()) {
                accessTokenOut = std::move(access);
                refreshTokenOut = std::move(refresh);
                expiresInSecondsOut = expiry;
            }
        }
    } else if (listener != INVALID_SOCKET) {
        CLOUD_CLOSE_SOCKET(listener);
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}

auto CloudSyncController::performUpload(const fs::path& path, std::string& fileIdInOut, std::string& folderIdInOut)
        -> std::string {
    std::string result = this->ensureAccessToken();
    if (!result.empty()) {
        return result;
    }

    result = this->ensureDriveFolderId(folderIdInOut);
    if (!result.empty()) {
        return result;
    }

    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        return FS(_F("could not read file: {1}") % path.string());
    }
    std::string fileData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::string access;
    {
        const std::lock_guard<std::mutex> lock{tokenMtx};
        access = this->accessToken;
    }

    std::string response;
    if (fileIdInOut.empty()) {
        std::string meta = "{\"name\":\"" + escapeJsonString(path.filename().string()) + "\",\"parents\":[\"" +
                           escapeJsonString(folderIdInOut) + "\"]}";
        std::string body = std::string("--") + MULTIPART_BOUNDARY +
                           "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" + meta + "\r\n--" +
                           MULTIPART_BOUNDARY +
                           "\r\nContent-Type: application/octet-stream\r\n\r\n" +
                           fileData + "\r\n--" + MULTIPART_BOUNDARY + "--\r\n";
        std::string multipartType = std::string("multipart/related; boundary=\"") + MULTIPART_BOUNDARY + "\"";
        result = this->httpCall(
                "POST", std::string(DRIVE_UPLOAD_URL) + "?uploadType=multipart&fields=id", body,
                multipartType, access, 600, response);
        if (result.empty()) {
            JsonDoc doc;
            result = jsonOpenDoc(response, doc);
            if (result.empty()) {
                fileIdInOut = jsonGetString(doc.obj, "id");
                if (fileIdInOut.empty()) {
                    result = "upload returned no file id";
                }
            }
        }
    } else {
        result = this->httpCall("PATCH", std::string(DRIVE_UPLOAD_URL) + "/" + fileIdInOut + "?uploadType=media",
                                fileData, "application/octet-stream", access, 600, response);
    }
    return result;
}
