/*
 * Xournal++
 *
 * Google Drive cloud sync controller (fork feature)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "filesystem.h"

class Control;
class Settings;
class CloudLoginJob;
class DriveUploadJob;
class DrivePdfUploadJob;

class CloudSyncController {
public:
    explicit CloudSyncController(Control* control);
    ~CloudSyncController();
    CloudSyncController(const CloudSyncController&) = delete;
    auto operator=(const CloudSyncController&) -> CloudSyncController& = delete;

    bool isConfigured() const;
    bool isLoggedIn() const;
    bool isLoginRunning() const;

    void login();
    void logout();
    void uploadDocument(const fs::path& path);
    void syncNow();

    void setStatusCallback(std::function<void(const std::string&)> cb);
    void clearStatusCallback();
    void setStatus(const std::string& status);

    void setConnectionCallback(std::function<void(bool)> cb);  // fork: menubar Drive indicator
    void clearConnectionCallback();
    auto getCredentialsPath() const -> std::string;

private:
    friend class CloudLoginJob;
    friend class DriveUploadJob;
    friend class DrivePdfUploadJob;

    // Called from job threads (network only, no UI access)
    auto performLogin(std::string& accessTokenOut, std::string& refreshTokenOut, long long& expiresInSecondsOut)
            -> std::string;
    auto performUpload(const fs::path& path, std::string& fileIdInOut, std::string& folderIdInOut) -> std::string;

    // Called from the main thread, e.g. from a job's afterRun()
    void setCloudTokens(const std::string& accessToken, const std::string& refreshToken, long long expiresInSeconds);
    void setCloudFileId(const fs::path& path, const std::string& fileId);
    void setCloudPdfFileId(const std::string& pdfName, const std::string& fileId);  // fork: PDF copy

    void notifyConnectionState(bool connected);  // fork: menubar Drive indicator
    void setCloudFolderId(const std::string& folderId);

    // Helpers (both main and job threads, unless noted)
    auto loadCredentials(std::string& clientIdOut, std::string& clientSecretOut) const -> bool;
    auto tokenRequest(const std::string& body, std::string& accessTokenOut, std::string& refreshTokenOut,
                      int& expiresInSecOut) -> std::string;
    auto refreshAccessToken() -> std::string;
    auto ensureAccessToken() -> std::string;
    auto ensureDriveFolderId(std::string& folderIdInOut) -> std::string;
    auto httpCall(const std::string& method, const std::string& url, const std::string& body,
                  const std::string& contentType, const std::string& bearerToken, long timeoutSec,
                  std::string& responseOut) -> std::string;

private:
    Control* control = nullptr;
    Settings* settings = nullptr;
    std::atomic<bool> loginRunning{false};

    mutable std::mutex tokenMtx;
    std::string accessToken;
    std::string refreshToken;
    long long accessExpireEpoch = 0;

    mutable std::mutex statusCbMtx;
    std::function<void(const std::string&)> statusCallback;

    // fork: callback for the menubar Drive connection indicator
    mutable std::mutex connectionCbMtx;
    std::function<void(bool)> connectionCallback;

    fs::path credentialsFile;
};
