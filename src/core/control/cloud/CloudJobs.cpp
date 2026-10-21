/*
 * Xournal++
 *
 * Background jobs for the Google Drive cloud sync (fork feature)
 *
 * @license GNU GPLv2 or later
 */

#include "CloudJobs.h"

#include <glib.h>

#include "control/Control.h"
#include "control/cloud/CloudSyncController.h"
#include "util/XojMsgBox.h"
#include "util/i18n.h"

CloudLoginJob::CloudLoginJob(CloudSyncController* controllerIn): controller(controllerIn) {}

CloudLoginJob::~CloudLoginJob() = default;

auto CloudLoginJob::getType() -> JobType { return JOB_TYPE_CLOUD; }

void CloudLoginJob::run() {
    this->error = this->controller->performLogin(this->accessToken, this->refreshToken, this->expiresInSeconds);
    callAfterRun();
}

void CloudLoginJob::afterRun() {
    if (!this->error.empty()) {
        this->controller->loginRunning.store(false);
        XojMsgBox::showErrorToUser(this->controller->control->getGtkWindow(),
                                   FS(_F("Google Drive login failed: {1}") % this->error));
        this->controller->setStatus(FS(_F("Google Drive login failed: {1}") % this->error));
    } else {
        this->controller->setCloudTokens(this->accessToken, this->refreshToken, this->expiresInSeconds);
        this->controller->setStatus(_("Google Drive: connected."));
    }
}

DriveUploadJob::DriveUploadJob(CloudSyncController* controllerIn, fs::path pathIn, std::string initialFolderId,
                               std::string initialFileId):
        controller(controllerIn),
        path(std::move(pathIn)),
        folderId(std::move(initialFolderId)),
        fileId(std::move(initialFileId)),
        initialFolderId(this->folderId),
        initialFileId(this->fileId) {}

DriveUploadJob::~DriveUploadJob() = default;

auto DriveUploadJob::getType() -> JobType { return JOB_TYPE_CLOUD; }

void DriveUploadJob::run() {
    this->error = this->controller->performUpload(this->path, this->fileId, this->folderId);
    callAfterRun();
}

void DriveUploadJob::afterRun() {
    if (!this->error.empty()) {
        std::string msg = FS(_F("Google Drive sync failed for {1}: {2}") % this->path.filename().string() %
                             this->error);
        XojMsgBox::showErrorToUser(this->controller->control->getGtkWindow(), msg);
        this->controller->setStatus(msg);
        return;
    }
    if (this->initialFileId.empty() && !this->fileId.empty()) {
        this->controller->setCloudFileId(this->path, this->fileId);
    }
    if (this->initialFolderId.empty() && !this->folderId.empty()) {
        this->controller->setCloudFolderId(this->folderId);
    }
    this->controller->setStatus(FS(_F("Google Drive: synced {1} ✓") % this->path.filename().string()));
}
