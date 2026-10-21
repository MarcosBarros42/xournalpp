/*
 * Xournal++
 *
 * Background jobs for the Google Drive cloud sync (fork feature)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <string>

#include "control/jobs/Job.h"

#include "filesystem.h"

class CloudSyncController;

class CloudLoginJob: public Job {
public:
    explicit CloudLoginJob(CloudSyncController* controller);
    ~CloudLoginJob() override;

    void run() override;
    void afterRun() override;
    auto getType() -> JobType override;

private:
    CloudSyncController* controller = nullptr;
    std::string accessToken;
    std::string refreshToken;
    long long expiresInSeconds = 0;
    std::string error;
};

class DriveUploadJob: public Job {
public:
    DriveUploadJob(CloudSyncController* controller, fs::path path, std::string initialFolderId,
                   std::string initialFileId);
    ~DriveUploadJob() override;

    void run() override;
    void afterRun() override;
    auto getType() -> JobType override;

private:
    CloudSyncController* controller = nullptr;
    fs::path path;
    std::string folderId;
    std::string fileId;
    std::string initialFolderId;
    std::string initialFileId;
    std::string error;
};

// fork: uploads a PDF copy of the document alongside the .xopp file
class DrivePdfUploadJob: public Job {
public:
    DrivePdfUploadJob(CloudSyncController* controllerIn, std::string pdfNameIn, std::string initialFolderId,
                      std::string initialFileId);
    ~DrivePdfUploadJob() override;

    void run() override;
    void afterRun() override;
    auto getType() -> JobType override;

private:
    CloudSyncController* controller = nullptr;
    std::string pdfName;
    fs::path pdfPath;
    std::string folderId;
    std::string fileId;
    std::string initialFolderId;
    std::string initialFileId;
    std::string error;
};
