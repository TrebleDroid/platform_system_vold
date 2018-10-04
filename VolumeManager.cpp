/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_PACKAGE_MANAGER

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>

#include <linux/kdev_t.h>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <cutils/fs.h>
#include <utils/Trace.h>

#include <selinux/android.h>

#include <sysutils/NetlinkEvent.h>

#include <private/android_filesystem_config.h>

#include <ext4_utils/ext4_crypt.h>

#include "Devmapper.h"
#include "Ext4Crypt.h"
#include "Loop.h"
#include "NetlinkManager.h"
#include "Process.h"
#include "Utils.h"
#include "VoldNativeService.h"
#include "VoldUtil.h"
#include "VolumeManager.h"
#include "cryptfs.h"
#include "fs/Ext4.h"
#include "fs/Vfat.h"
#include "model/EmulatedVolume.h"
#include "model/ObbVolume.h"

using android::base::GetBoolProperty;
using android::base::StartsWith;
using android::base::StringAppendF;
using android::base::StringPrintf;
using android::base::unique_fd;

static const char* kPathUserMount = "/mnt/user";
static const char* kPathVirtualDisk = "/data/misc/vold/virtual_disk";

static const char* kIsolatedStorage = "persist.sys.isolated_storage";
static const char* kPropVirtualDisk = "persist.sys.virtual_disk";

static const std::string kEmptyString("");

/* 512MiB is large enough for testing purposes */
static const unsigned int kSizeVirtualDisk = 536870912;

static const unsigned int kMajorBlockMmc = 179;
static const unsigned int kMajorBlockExperimentalMin = 240;
static const unsigned int kMajorBlockExperimentalMax = 254;

VolumeManager* VolumeManager::sInstance = NULL;

VolumeManager* VolumeManager::Instance() {
    if (!sInstance) sInstance = new VolumeManager();
    return sInstance;
}

VolumeManager::VolumeManager() {
    mDebug = false;
    mNextObbId = 0;
    // For security reasons, assume that a secure keyguard is
    // showing until we hear otherwise
    mSecureKeyguardShowing = true;
}

VolumeManager::~VolumeManager() {}

int VolumeManager::updateVirtualDisk() {
    ATRACE_NAME("VolumeManager::updateVirtualDisk");
    if (GetBoolProperty(kPropVirtualDisk, false)) {
        if (access(kPathVirtualDisk, F_OK) != 0) {
            Loop::createImageFile(kPathVirtualDisk, kSizeVirtualDisk / 512);
        }

        if (mVirtualDisk == nullptr) {
            if (Loop::create(kPathVirtualDisk, mVirtualDiskPath) != 0) {
                LOG(ERROR) << "Failed to create virtual disk";
                return -1;
            }

            struct stat buf;
            if (stat(mVirtualDiskPath.c_str(), &buf) < 0) {
                PLOG(ERROR) << "Failed to stat " << mVirtualDiskPath;
                return -1;
            }

            auto disk = new android::vold::Disk(
                "virtual", buf.st_rdev, "virtual",
                android::vold::Disk::Flags::kAdoptable | android::vold::Disk::Flags::kSd);
            mVirtualDisk = std::shared_ptr<android::vold::Disk>(disk);
            handleDiskAdded(mVirtualDisk);
        }
    } else {
        if (mVirtualDisk != nullptr) {
            dev_t device = mVirtualDisk->getDevice();
            handleDiskRemoved(device);

            Loop::destroyByDevice(mVirtualDiskPath.c_str());
            mVirtualDisk = nullptr;
        }

        if (access(kPathVirtualDisk, F_OK) == 0) {
            unlink(kPathVirtualDisk);
        }
    }
    return 0;
}

int VolumeManager::setDebug(bool enable) {
    mDebug = enable;
    return 0;
}

int VolumeManager::start() {
    ATRACE_NAME("VolumeManager::start");

    // Always start from a clean slate by unmounting everything in
    // directories that we own, in case we crashed.
    unmountAll();

    Devmapper::destroyAll();
    Loop::destroyAll();

    // Assume that we always have an emulated volume on internal
    // storage; the framework will decide if it should be mounted.
    CHECK(mInternalEmulated == nullptr);
    mInternalEmulated = std::shared_ptr<android::vold::VolumeBase>(
        new android::vold::EmulatedVolume("/data/media"));
    mInternalEmulated->create();

    // Consider creating a virtual disk
    updateVirtualDisk();

    return 0;
}

int VolumeManager::stop() {
    CHECK(mInternalEmulated != nullptr);
    mInternalEmulated->destroy();
    mInternalEmulated = nullptr;
    return 0;
}

void VolumeManager::handleBlockEvent(NetlinkEvent* evt) {
    std::lock_guard<std::mutex> lock(mLock);

    if (mDebug) {
        LOG(DEBUG) << "----------------";
        LOG(DEBUG) << "handleBlockEvent with action " << (int)evt->getAction();
        evt->dump();
    }

    std::string eventPath(evt->findParam("DEVPATH") ? evt->findParam("DEVPATH") : "");
    std::string devType(evt->findParam("DEVTYPE") ? evt->findParam("DEVTYPE") : "");

    if (devType != "disk") return;

    int major = std::stoi(evt->findParam("MAJOR"));
    int minor = std::stoi(evt->findParam("MINOR"));
    dev_t device = makedev(major, minor);

    switch (evt->getAction()) {
        case NetlinkEvent::Action::kAdd: {
            for (const auto& source : mDiskSources) {
                if (source->matches(eventPath)) {
                    // For now, assume that MMC and virtio-blk (the latter is
                    // emulator-specific; see Disk.cpp for details) devices are SD,
                    // and that everything else is USB
                    int flags = source->getFlags();
                    if (major == kMajorBlockMmc || (android::vold::IsRunningInEmulator() &&
                                                    major >= (int)kMajorBlockExperimentalMin &&
                                                    major <= (int)kMajorBlockExperimentalMax)) {
                        flags |= android::vold::Disk::Flags::kSd;
                    } else {
                        flags |= android::vold::Disk::Flags::kUsb;
                    }

                    auto disk =
                        new android::vold::Disk(eventPath, device, source->getNickname(), flags);
                    handleDiskAdded(std::shared_ptr<android::vold::Disk>(disk));
                    break;
                }
            }
            break;
        }
        case NetlinkEvent::Action::kChange: {
            LOG(DEBUG) << "Disk at " << major << ":" << minor << " changed";
            handleDiskChanged(device);
            break;
        }
        case NetlinkEvent::Action::kRemove: {
            handleDiskRemoved(device);
            break;
        }
        default: {
            LOG(WARNING) << "Unexpected block event action " << (int)evt->getAction();
            break;
        }
    }
}

void VolumeManager::handleDiskAdded(const std::shared_ptr<android::vold::Disk>& disk) {
    // For security reasons, if secure keyguard is showing, wait
    // until the user unlocks the device to actually touch it
    if (mSecureKeyguardShowing) {
        LOG(INFO) << "Found disk at " << disk->getEventPath()
                  << " but delaying scan due to secure keyguard";
        mPendingDisks.push_back(disk);
    } else {
        disk->create();
        mDisks.push_back(disk);
    }
}

void VolumeManager::handleDiskChanged(dev_t device) {
    for (const auto& disk : mDisks) {
        if (disk->getDevice() == device) {
            disk->readMetadata();
            disk->readPartitions();
        }
    }

    // For security reasons, we ignore all pending disks, since
    // we'll scan them once the device is unlocked
}

void VolumeManager::handleDiskRemoved(dev_t device) {
    auto i = mDisks.begin();
    while (i != mDisks.end()) {
        if ((*i)->getDevice() == device) {
            (*i)->destroy();
            i = mDisks.erase(i);
        } else {
            ++i;
        }
    }
    auto j = mPendingDisks.begin();
    while (j != mPendingDisks.end()) {
        if ((*j)->getDevice() == device) {
            j = mPendingDisks.erase(j);
        } else {
            ++j;
        }
    }
}

void VolumeManager::addDiskSource(const std::shared_ptr<DiskSource>& diskSource) {
    std::lock_guard<std::mutex> lock(mLock);
    mDiskSources.push_back(diskSource);
}

std::shared_ptr<android::vold::Disk> VolumeManager::findDisk(const std::string& id) {
    for (auto disk : mDisks) {
        if (disk->getId() == id) {
            return disk;
        }
    }
    return nullptr;
}

std::shared_ptr<android::vold::VolumeBase> VolumeManager::findVolume(const std::string& id) {
    // Vold could receive "mount" after "shutdown" command in the extreme case.
    // If this happens, mInternalEmulated will equal nullptr and
    // we need to deal with it in order to avoid null pointer crash.
    if (mInternalEmulated != nullptr && mInternalEmulated->getId() == id) {
        return mInternalEmulated;
    }
    for (const auto& disk : mDisks) {
        auto vol = disk->findVolume(id);
        if (vol != nullptr) {
            return vol;
        }
    }
    for (const auto& vol : mObbVolumes) {
        if (vol->getId() == id) {
            return vol;
        }
    }
    return nullptr;
}

void VolumeManager::listVolumes(android::vold::VolumeBase::Type type, std::list<std::string>& list) {
    list.clear();
    for (const auto& disk : mDisks) {
        disk->listVolumes(type, list);
    }
}

int VolumeManager::forgetPartition(const std::string& partGuid, const std::string& fsUuid) {
    std::string normalizedGuid;
    if (android::vold::NormalizeHex(partGuid, normalizedGuid)) {
        LOG(WARNING) << "Invalid GUID " << partGuid;
        return -1;
    }

    bool success = true;
    std::string keyPath = android::vold::BuildKeyPath(normalizedGuid);
    if (unlink(keyPath.c_str()) != 0) {
        LOG(ERROR) << "Failed to unlink " << keyPath;
        success = false;
    }
    if (e4crypt_is_native()) {
        if (!e4crypt_destroy_volume_keys(fsUuid)) {
            success = false;
        }
    }
    return success ? 0 : -1;
}

int VolumeManager::linkPrimary(userid_t userId) {
    std::string source(mPrimary->getPath());
    if (mPrimary->isEmulated()) {
        source = StringPrintf("%s/%d", source.c_str(), userId);
        fs_prepare_dir(source.c_str(), 0755, AID_ROOT, AID_ROOT);
    }

    std::string target(StringPrintf("/mnt/user/%d/primary", userId));
    if (TEMP_FAILURE_RETRY(unlink(target.c_str()))) {
        if (errno != ENOENT) {
            PLOG(WARNING) << "Failed to unlink " << target;
        }
    }
    LOG(DEBUG) << "Linking " << source << " to " << target;
    if (TEMP_FAILURE_RETRY(symlink(source.c_str(), target.c_str()))) {
        PLOG(WARNING) << "Failed to link";
        return -errno;
    }
    return 0;
}

int VolumeManager::mountPkgSpecificDir(const std::string& mntSourceRoot,
                                       const std::string& mntTargetRoot,
                                       const std::string& packageName, const char* dirName) {
    std::string mntSourceDir =
        StringPrintf("%s/Android/%s/%s", mntSourceRoot.c_str(), dirName, packageName.c_str());
    std::string mntTargetDir =
        StringPrintf("%s/Android/%s/%s", mntTargetRoot.c_str(), dirName, packageName.c_str());
    if (umount2(mntTargetDir.c_str(), MNT_DETACH) == -1 && errno != EINVAL && errno != ENOENT) {
        PLOG(ERROR) << "Failed to unmount " << mntTargetDir;
        return -1;
    }
    if (TEMP_FAILURE_RETRY(mount(mntSourceDir.c_str(), mntTargetDir.c_str(), nullptr,
                                 MS_BIND | MS_REC, nullptr)) == -1) {
        PLOG(ERROR) << "Failed to mount " << mntSourceDir << " to " << mntTargetDir;
        return -1;
    }
    if (TEMP_FAILURE_RETRY(
            mount(nullptr, mntTargetDir.c_str(), nullptr, MS_REC | MS_SLAVE, nullptr)) == -1) {
        PLOG(ERROR) << "Failed to set MS_SLAVE at " << mntTargetDir;
        return -1;
    }
    return 0;
}

int VolumeManager::mountPkgSpecificDirsForRunningProcs(
    userid_t userId, const std::vector<std::string>& packageNames,
    const std::vector<std::string>& visibleVolLabels) {
    // TODO: New processes could be started while traversing over the existing
    // processes which would end up not having the necessary bind mounts. This
    // issue needs to be fixed, may be by doing multiple passes here?
    std::unique_ptr<DIR, decltype(&closedir)> dirp(opendir("/proc"), closedir);
    if (!dirp) {
        PLOG(ERROR) << "Failed to opendir /proc";
        return -1;
    }

    std::string rootName;
    // Figure out root namespace to compare against below
    if (!android::vold::Readlinkat(dirfd(dirp.get()), "1/ns/mnt", &rootName)) {
        PLOG(ERROR) << "Failed to read root namespace";
        return -1;
    }

    struct stat fullWriteSb;
    if (TEMP_FAILURE_RETRY(stat("/mnt/runtime/write", &fullWriteSb)) == -1) {
        PLOG(ERROR) << "Failed to stat /mnt/runtime/write";
        return -1;
    }

    std::unordered_set<appid_t> validAppIds;
    for (auto& package : packageNames) {
        validAppIds.insert(mAppIds[package]);
    }
    std::vector<std::string>& userPackages = mUserPackages[userId];

    struct dirent* de;
    // Poke through all running PIDs look for apps running in userId
    while ((de = readdir(dirp.get()))) {
        pid_t pid;
        if (de->d_type != DT_DIR) continue;
        if (!android::base::ParseInt(de->d_name, &pid)) continue;

        const unique_fd pidFd(
            openat(dirfd(dirp.get()), de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (pidFd.get() < 0) {
            PLOG(WARNING) << "Failed to open /proc/" << pid;
            continue;
        }
        struct stat sb;
        if (fstat(pidFd.get(), &sb) != 0) {
            PLOG(WARNING) << "Failed to stat " << de->d_name;
            continue;
        }
        if (multiuser_get_user_id(sb.st_uid) != userId) {
            continue;
        }

        // Matches so far, but refuse to touch if in root namespace
        LOG(VERBOSE) << "Found matching PID " << de->d_name;
        std::string pidName;
        if (!android::vold::Readlinkat(pidFd.get(), "ns/mnt", &pidName)) {
            PLOG(WARNING) << "Failed to read namespace for " << de->d_name;
            continue;
        }
        if (rootName == pidName) {
            LOG(WARNING) << "Skipping due to root namespace";
            continue;
        }

        // Only update the mount points of processes running with one of validAppIds.
        // This should skip any isolated uids.
        appid_t appId = multiuser_get_app_id(sb.st_uid);
        if (validAppIds.find(appId) == validAppIds.end()) {
            continue;
        }

        std::vector<std::string> packagesForUid;
        for (auto& package : userPackages) {
            if (mAppIds[package] == appId) {
                packagesForUid.push_back(package);
            }
        }
        if (packagesForUid.empty()) {
            continue;
        }
        const std::string& sandboxId = mSandboxIds[appId];

        // We purposefully leave the namespace open across the fork
        unique_fd nsFd(openat(pidFd.get(), "ns/mnt", O_RDONLY));  // not O_CLOEXEC
        if (nsFd.get() < 0) {
            PLOG(WARNING) << "Failed to open namespace for " << de->d_name;
            continue;
        }

        pid_t child;
        if (!(child = fork())) {
            if (setns(nsFd.get(), CLONE_NEWNS) != 0) {
                PLOG(ERROR) << "Failed to setns for " << de->d_name;
                _exit(1);
            }

            struct stat storageSb;
            if (TEMP_FAILURE_RETRY(stat("/storage", &storageSb)) == -1) {
                PLOG(ERROR) << "Failed to stat /storage";
                _exit(1);
            }

            // Some packages have access to full external storage, identify processes belonging
            // to those packages by comparing inode no.s of /mnt/runtime/write and /storage
            if (storageSb.st_dev == fullWriteSb.st_dev && storageSb.st_ino == fullWriteSb.st_ino) {
                _exit(0);
            } else {
                // Some packages don't have access to external storage and processes belonging to
                // those packages don't have anything mounted at /storage. So, identify those
                // processes by comparing inode no.s of /mnt/user/%d/package/%s
                // and /storage
                std::string pkgStorageSource;
                for (auto& package : packagesForUid) {
                    std::string sandbox =
                        StringPrintf("/mnt/user/%d/package/%s", userId, package.c_str());
                    struct stat s;
                    if (TEMP_FAILURE_RETRY(stat(sandbox.c_str(), &s)) == -1) {
                        PLOG(ERROR) << "Failed to stat " << sandbox;
                        _exit(1);
                    }
                    if (storageSb.st_dev == s.st_dev && storageSb.st_ino == s.st_ino) {
                        pkgStorageSource = sandbox;
                        break;
                    }
                }
                if (pkgStorageSource.empty()) {
                    _exit(0);
                }
            }

            for (auto& volumeLabel : visibleVolLabels) {
                std::string mntSource = StringPrintf("/mnt/runtime/write/%s", volumeLabel.c_str());
                std::string mntTarget = StringPrintf("/storage/%s", volumeLabel.c_str());
                if (volumeLabel == "emulated") {
                    StringAppendF(&mntSource, "/%d", userId);
                    StringAppendF(&mntTarget, "/%d", userId);
                }
                for (auto& package : packagesForUid) {
                    mountPkgSpecificDir(mntSource, mntTarget, package, "data");
                    mountPkgSpecificDir(mntSource, mntTarget, package, "media");
                    mountPkgSpecificDir(mntSource, mntTarget, package, "obb");
                }
            }
            _exit(0);
        }

        if (child == -1) {
            PLOG(ERROR) << "Failed to fork";
        } else {
            TEMP_FAILURE_RETRY(waitpid(child, nullptr, 0));
        }
    }
    return 0;
}

int VolumeManager::prepareSandboxes(userid_t userId, const std::vector<std::string>& packageNames,
                                    const std::vector<std::string>& visibleVolLabels) {
    if (visibleVolLabels.empty()) {
        return 0;
    }
    for (auto& volumeLabel : visibleVolLabels) {
        std::string volumeRoot(StringPrintf("/mnt/runtime/write/%s", volumeLabel.c_str()));
        bool isVolPrimaryEmulated = (volumeLabel == mPrimary->getLabel() && mPrimary->isEmulated());
        if (isVolPrimaryEmulated) {
            StringAppendF(&volumeRoot, "/%d", userId);
            if (fs_prepare_dir(volumeRoot.c_str(), 0755, AID_ROOT, AID_ROOT) != 0) {
                PLOG(ERROR) << "fs_prepare_dir failed on " << volumeRoot;
                return -errno;
            }
        }

        std::string sandboxRoot =
            prepareSubDirs(volumeRoot, "Android/sandbox/", 0700, AID_ROOT, AID_ROOT);
        if (sandboxRoot.empty()) {
            return -errno;
        }
        std::string sharedSandboxRoot;
        StringAppendF(&sharedSandboxRoot, "%s/shared", sandboxRoot.c_str());
        // Create shared sandbox base dir for apps with sharedUserIds
        if (fs_prepare_dir(sharedSandboxRoot.c_str(), 0700, AID_ROOT, AID_ROOT) != 0) {
            PLOG(ERROR) << "fs_prepare_dir failed on " << sharedSandboxRoot;
            return -errno;
        }

        if (!createPkgSpecificDirRoots(volumeRoot)) {
            return -errno;
        }

        std::string mntTargetRoot = StringPrintf("/mnt/user/%d", userId);
        if (fs_prepare_dir(mntTargetRoot.c_str(), 0751, AID_ROOT, AID_ROOT) != 0) {
            PLOG(ERROR) << "fs_prepare_dir failed on " << mntTargetRoot;
            return -errno;
        }
        mntTargetRoot.append("/package");
        if (fs_prepare_dir(mntTargetRoot.c_str(), 0700, AID_ROOT, AID_ROOT) != 0) {
            PLOG(ERROR) << "fs_prepare_dir failed on " << mntTargetRoot;
            return -errno;
        }

        for (auto& packageName : packageNames) {
            const auto& it = mAppIds.find(packageName);
            if (it == mAppIds.end()) {
                PLOG(ERROR) << "appId is not available for " << packageName;
                continue;
            }
            appid_t appId = it->second;
            std::string sandboxId = mSandboxIds[appId];
            uid_t uid = multiuser_get_uid(userId, appId);

            // [1] Create /mnt/runtime/write/emulated/0/Android/sandbox/<sandboxId>
            // [2] Create /mnt/user/0/package/<packageName>/emulated/0
            // Mount [1] at [2]
            std::string pkgSandboxSourceDir = prepareSandboxSource(uid, sandboxId, sandboxRoot);
            if (pkgSandboxSourceDir.empty()) {
                return -errno;
            }
            std::string pkgSandboxTargetDir = prepareSandboxTarget(
                packageName, uid, volumeLabel, mntTargetRoot, isVolPrimaryEmulated);
            if (pkgSandboxTargetDir.empty()) {
                return -errno;
            }
            if (umount2(pkgSandboxTargetDir.c_str(), MNT_DETACH) == -1 && errno != EINVAL &&
                errno != ENOENT) {
                PLOG(ERROR) << "Failed to unmount " << pkgSandboxTargetDir;
                return -errno;
            }
            if (TEMP_FAILURE_RETRY(mount(pkgSandboxSourceDir.c_str(), pkgSandboxTargetDir.c_str(),
                                         nullptr, MS_BIND | MS_REC, nullptr)) == -1) {
                PLOG(ERROR) << "Failed to mount " << pkgSandboxSourceDir << " at "
                            << pkgSandboxTargetDir;
                return -errno;
            }
            if (TEMP_FAILURE_RETRY(mount(nullptr, pkgSandboxTargetDir.c_str(), nullptr,
                                         MS_SLAVE | MS_REC, nullptr)) == -1) {
                PLOG(ERROR) << "Failed to mount " << pkgSandboxSourceDir << " at "
                            << pkgSandboxTargetDir;
                return -errno;
            }

            // Create Android/{data,media,obb}/<packageName> segments at
            // [1] /mnt/runtime/write/emulated/0/ and
            // [2] /mnt/runtime/write/emulated/0/Android/sandbox/<sandboxId>/emulated/0/
            if (!createPkgSpecificDirs(packageName, uid, volumeRoot, pkgSandboxSourceDir)) {
                return -errno;
            }

            if (volumeLabel == mPrimary->getLabel()) {
                // Create [1] /mnt/user/0/package/<packageName>/self/
                // Already created [2] /mnt/user/0/package/<packageName>/emulated/0
                // Mount [2] at [1]
                std::string pkgPrimaryTargetDir =
                    StringPrintf("%s/%s/self", mntTargetRoot.c_str(), packageName.c_str());
                if (fs_prepare_dir(pkgPrimaryTargetDir.c_str(), 0755, uid, uid) != 0) {
                    PLOG(ERROR) << "Failed to fs_prepare_dir on " << pkgPrimaryTargetDir;
                    return -errno;
                }
                StringAppendF(&pkgPrimaryTargetDir, "/primary");
                std::string primarySource(mPrimary->getPath());
                if (isVolPrimaryEmulated) {
                    StringAppendF(&primarySource, "/%d", userId);
                }
                if (TEMP_FAILURE_RETRY(unlink(pkgPrimaryTargetDir.c_str()))) {
                    if (errno != ENOENT) {
                        PLOG(ERROR) << "Failed to unlink " << pkgPrimaryTargetDir;
                    }
                }
                if (TEMP_FAILURE_RETRY(symlink(primarySource.c_str(), pkgPrimaryTargetDir.c_str()))) {
                    PLOG(ERROR) << "Failed to link " << primarySource << " at "
                                << pkgPrimaryTargetDir;
                    return -errno;
                }
            }
        }
    }
    mountPkgSpecificDirsForRunningProcs(userId, packageNames, visibleVolLabels);
    return 0;
}

std::string VolumeManager::prepareSubDirs(const std::string& pathPrefix, const std::string& subDirs,
                                          mode_t mode, uid_t uid, gid_t gid) {
    std::string path(pathPrefix);
    std::vector<std::string> subDirList = android::base::Split(subDirs, "/");
    for (size_t i = 0; i < subDirList.size(); ++i) {
        std::string subDir = subDirList[i];
        if (subDir.empty()) {
            continue;
        }
        StringAppendF(&path, "/%s", subDir.c_str());
        if (fs_prepare_dir(path.c_str(), mode, uid, gid) != 0) {
            PLOG(ERROR) << "fs_prepare_dir failed on " << path;
            return kEmptyString;
        }
    }
    return path;
}

std::string VolumeManager::prepareSandboxSource(uid_t uid, const std::string& sandboxId,
                                                const std::string& sandboxRootDir) {
    std::string sandboxSourceDir(sandboxRootDir);
    if (StartsWith(sandboxId, "shared:")) {
        StringAppendF(&sandboxSourceDir, "/shared/%s", sandboxId.substr(7).c_str());
    } else {
        StringAppendF(&sandboxSourceDir, "/%s", sandboxId.c_str());
    }
    if (fs_prepare_dir(sandboxSourceDir.c_str(), 0755, uid, uid) != 0) {
        PLOG(ERROR) << "fs_prepare_dir failed on " << sandboxSourceDir;
        return kEmptyString;
    }
    return sandboxSourceDir;
}

std::string VolumeManager::prepareSandboxTarget(const std::string& packageName, uid_t uid,
                                                const std::string& volumeLabel,
                                                const std::string& mntTargetRootDir,
                                                bool isUserDependent) {
    std::string segment;
    if (isUserDependent) {
        segment = StringPrintf("%s/%s/%d/", packageName.c_str(), volumeLabel.c_str(),
                               multiuser_get_user_id(uid));
    } else {
        segment = StringPrintf("%s/%s/", packageName.c_str(), volumeLabel.c_str());
    }
    return prepareSubDirs(mntTargetRootDir, segment.c_str(), 0755, uid, uid);
}

std::string VolumeManager::preparePkgDataSource(const std::string& packageName, uid_t uid,
                                                const std::string& dataRootDir) {
    std::string dataSourceDir = StringPrintf("%s/%s", dataRootDir.c_str(), packageName.c_str());
    if (fs_prepare_dir(dataSourceDir.c_str(), 0755, uid, uid) != 0) {
        PLOG(ERROR) << "fs_prepare_dir failed on " << dataSourceDir;
        return kEmptyString;
    }
    return dataSourceDir;
}

bool VolumeManager::createPkgSpecificDirRoots(const std::string& volumeRoot) {
    std::string volumeAndroidRoot = StringPrintf("%s/Android", volumeRoot.c_str());
    if (fs_prepare_dir(volumeAndroidRoot.c_str(), 0700, AID_ROOT, AID_ROOT) != 0) {
        PLOG(ERROR) << "fs_prepare_dir failed on " << volumeAndroidRoot;
        return false;
    }
    std::array<std::string, 3> dirs = {"data", "media", "obb"};
    for (auto& dir : dirs) {
        std::string path = StringPrintf("%s/%s", volumeAndroidRoot.c_str(), dir.c_str());
        if (fs_prepare_dir(path.c_str(), 0700, AID_ROOT, AID_ROOT) != 0) {
            PLOG(ERROR) << "fs_prepare_dir failed on " << path;
            return false;
        }
    }
    return true;
}

bool VolumeManager::createPkgSpecificDirs(const std::string& packageName, uid_t uid,
                                          const std::string& volumeRoot,
                                          const std::string& sandboxDirRoot) {
    std::array<std::string, 3> dirs = {"data", "media", "obb"};
    for (auto& dir : dirs) {
        std::string sourceDir = StringPrintf("%s/Android/%s", volumeRoot.c_str(), dir.c_str());
        if (prepareSubDirs(sourceDir, packageName, 0755, uid, uid).empty()) {
            return false;
        }
        std::string sandboxSegment =
            StringPrintf("Android/%s/%s/", dir.c_str(), packageName.c_str());
        if (prepareSubDirs(sandboxDirRoot, sandboxSegment, 0755, uid, uid).empty()) {
            return false;
        }
    }
    return true;
}

int VolumeManager::onUserAdded(userid_t userId, int userSerialNumber) {
    mAddedUsers[userId] = userSerialNumber;
    return 0;
}

int VolumeManager::onUserRemoved(userid_t userId) {
    mAddedUsers.erase(userId);
    return 0;
}

int VolumeManager::onUserStarted(userid_t userId, const std::vector<std::string>& packageNames) {
    LOG(VERBOSE) << "onUserStarted: " << userId;
    // Note that sometimes the system will spin up processes from Zygote
    // before actually starting the user, so we're okay if Zygote
    // already created this directory.
    std::string path(StringPrintf("%s/%d", kPathUserMount, userId));
    fs_prepare_dir(path.c_str(), 0755, AID_ROOT, AID_ROOT);

    mStartedUsers.insert(userId);
    mUserPackages[userId] = packageNames;
    if (mPrimary) {
        linkPrimary(userId);
    }
    if (GetBoolProperty(kIsolatedStorage, false)) {
        std::vector<std::string> visibleVolLabels;
        for (auto& volId : mVisibleVolumeIds) {
            auto vol = findVolume(volId);
            userid_t mountUserId = vol->getMountUserId();
            if (mountUserId == userId || vol->isEmulated()) {
                visibleVolLabels.push_back(vol->getLabel());
            }
        }
        if (prepareSandboxes(userId, packageNames, visibleVolLabels) != 0) {
            return -errno;
        }
    }
    return 0;
}

int VolumeManager::onUserStopped(userid_t userId) {
    LOG(VERBOSE) << "onUserStopped: " << userId;
    mStartedUsers.erase(userId);

    std::string mntTargetDir = StringPrintf("/mnt/user/%d", userId);
    if (android::vold::UnmountTree(mntTargetDir) != 0) {
        PLOG(ERROR) << "unmountTree on " << mntTargetDir << " failed";
        return -errno;
    }
    if (android::vold::DeleteDirContentsAndDir(mntTargetDir) < 0) {
        PLOG(ERROR) << "DeleteDirContentsAndDir failed on " << mntTargetDir;
        return -errno;
    }
    LOG(VERBOSE) << "Success: DeleteDirContentsAndDir on " << mntTargetDir;
    return 0;
}

int VolumeManager::addAppIds(const std::vector<std::string>& packageNames,
                             const std::vector<int32_t>& appIds) {
    for (size_t i = 0; i < packageNames.size(); ++i) {
        mAppIds[packageNames[i]] = appIds[i];
    }
    return 0;
}

int VolumeManager::addSandboxIds(const std::vector<int32_t>& appIds,
                                 const std::vector<std::string>& sandboxIds) {
    for (size_t i = 0; i < appIds.size(); ++i) {
        mSandboxIds[appIds[i]] = sandboxIds[i];
    }
    return 0;
}

int VolumeManager::prepareSandboxForApp(const std::string& packageName, appid_t appId,
                                        const std::string& sandboxId, userid_t userId) {
    if (!GetBoolProperty(kIsolatedStorage, false)) {
        return 0;
    } else if (mStartedUsers.find(userId) == mStartedUsers.end()) {
        // User not started, no need to do anything now. Required bind mounts for the package will
        // be created when the user starts.
        return 0;
    }
    LOG(VERBOSE) << "prepareSandboxForApp: " << packageName << ", appId=" << appId
                 << ", sandboxId=" << sandboxId << ", userId=" << userId;
    mUserPackages[userId].push_back(packageName);
    mAppIds[packageName] = appId;
    mSandboxIds[appId] = sandboxId;

    std::vector<std::string> visibleVolLabels;
    for (auto& volId : mVisibleVolumeIds) {
        auto vol = findVolume(volId);
        userid_t mountUserId = vol->getMountUserId();
        if (mountUserId == userId || vol->isEmulated()) {
            visibleVolLabels.push_back(vol->getLabel());
        }
    }
    return prepareSandboxes(userId, {packageName}, visibleVolLabels);
}

int VolumeManager::destroySandboxForApp(const std::string& packageName, appid_t appId,
                                        const std::string& sandboxId, userid_t userId) {
    if (!GetBoolProperty(kIsolatedStorage, false)) {
        return 0;
    }
    LOG(VERBOSE) << "destroySandboxForApp: " << packageName << ", appId=" << appId
                 << ", sandboxId=" << sandboxId << ", userId=" << userId;
    auto& userPackages = mUserPackages[userId];
    std::remove(userPackages.begin(), userPackages.end(), packageName);
    // If the package is not uninstalled in any other users, remove appId and sandboxId
    // corresponding to it from the internal state.
    bool installedInAnyUser = false;
    for (auto& it : mUserPackages) {
        auto& packages = it.second;
        if (std::find(packages.begin(), packages.end(), packageName) != packages.end()) {
            installedInAnyUser = true;
            break;
        }
    }
    if (!installedInAnyUser) {
        mAppIds.erase(packageName);
        mSandboxIds.erase(appId);
    }

    std::vector<std::string> visibleVolLabels;
    for (auto& volId : mVisibleVolumeIds) {
        auto vol = findVolume(volId);
        userid_t mountUserId = vol->getMountUserId();
        if (mountUserId == userId || vol->isEmulated()) {
            if (destroySandboxForAppOnVol(packageName, sandboxId, userId, vol->getLabel()) < 0) {
                return -errno;
            }
        }
    }
    return 0;
}

int VolumeManager::destroySandboxForAppOnVol(const std::string& packageName,
                                             const std::string& sandboxId, userid_t userId,
                                             const std::string& volLabel) {
    LOG(VERBOSE) << "destroySandboxOnVol: " << packageName << ", userId=" << userId
                 << ", volLabel=" << volLabel;
    std::string pkgSandboxTarget =
        StringPrintf("/mnt/user/%d/package/%s", userId, packageName.c_str());
    if (android::vold::UnmountTree(pkgSandboxTarget)) {
        PLOG(ERROR) << "UnmountTree failed on " << pkgSandboxTarget;
    }

    std::string sandboxDir = StringPrintf("/mnt/runtime/write/%s", volLabel.c_str());
    if (volLabel == mPrimary->getLabel() && mPrimary->isEmulated()) {
        StringAppendF(&sandboxDir, "/%d", userId);
    }
    if (StartsWith(sandboxId, "shared:")) {
        StringAppendF(&sandboxDir, "/Android/sandbox/shared/%s", sandboxId.substr(7).c_str());
    } else {
        StringAppendF(&sandboxDir, "/Android/sandbox/%s", sandboxId.c_str());
    }

    if (android::vold::DeleteDirContentsAndDir(sandboxDir) < 0) {
        PLOG(ERROR) << "DeleteDirContentsAndDir failed on " << sandboxDir;
        return -errno;
    }
    return 0;
}

int VolumeManager::onSecureKeyguardStateChanged(bool isShowing) {
    mSecureKeyguardShowing = isShowing;
    if (!mSecureKeyguardShowing) {
        // Now that secure keyguard has been dismissed, process
        // any pending disks
        for (const auto& disk : mPendingDisks) {
            disk->create();
            mDisks.push_back(disk);
        }
        mPendingDisks.clear();
    }
    return 0;
}

int VolumeManager::onVolumeMounted(android::vold::VolumeBase* vol) {
    if (!GetBoolProperty(kIsolatedStorage, false)) {
        return 0;
    }

    if ((vol->getMountFlags() & android::vold::VoldNativeService::MOUNT_FLAG_VISIBLE) == 0) {
        return 0;
    }

    mVisibleVolumeIds.insert(vol->getId());
    userid_t mountUserId = vol->getMountUserId();
    if ((vol->getMountFlags() & android::vold::VoldNativeService::MOUNT_FLAG_PRIMARY) != 0) {
        // We don't want to create another shared_ptr here because then we will end up with
        // two shared_ptrs owning the underlying pointer without sharing it.
        mPrimary = findVolume(vol->getId());
        for (userid_t userId : mStartedUsers) {
            if (linkPrimary(userId) != 0) {
                return -errno;
            }
        }
    }
    if (vol->isEmulated()) {
        for (userid_t userId : mStartedUsers) {
            if (prepareSandboxes(userId, mUserPackages[userId], {vol->getLabel()}) != 0) {
                return -errno;
            }
        }
    } else if (mStartedUsers.find(mountUserId) != mStartedUsers.end()) {
        if (prepareSandboxes(mountUserId, mUserPackages[mountUserId], {vol->getLabel()}) != 0) {
            return -errno;
        }
    }
    return 0;
}

int VolumeManager::onVolumeUnmounted(android::vold::VolumeBase* vol) {
    if (!GetBoolProperty(kIsolatedStorage, false)) {
        return 0;
    }

    if (mVisibleVolumeIds.erase(vol->getId()) == 0) {
        return 0;
    }

    if ((vol->getMountFlags() & android::vold::VoldNativeService::MOUNT_FLAG_PRIMARY) != 0) {
        mPrimary = nullptr;
    }

    LOG(VERBOSE) << "visibleVolumeUnmounted: " << vol;
    userid_t mountUserId = vol->getMountUserId();
    if (vol->isEmulated()) {
        for (userid_t userId : mStartedUsers) {
            if (destroySandboxesForVol(vol, userId) != 0) {
                return -errno;
            }
        }
    } else if (mStartedUsers.find(mountUserId) != mStartedUsers.end()) {
        if (destroySandboxesForVol(vol, mountUserId) != 0) {
            return -errno;
        }
    }
    return 0;
}

int VolumeManager::destroySandboxesForVol(android::vold::VolumeBase* vol, userid_t userId) {
    LOG(VERBOSE) << "destroysandboxesForVol: " << vol << " for user=" << userId;
    const std::vector<std::string>& packageNames = mUserPackages[userId];
    for (auto& packageName : packageNames) {
        std::string volSandboxRoot = StringPrintf("/mnt/user/%d/package/%s/%s", userId,
                                                  packageName.c_str(), vol->getLabel().c_str());
        if (android::vold::UnmountTree(volSandboxRoot) != 0) {
            PLOG(ERROR) << "unmountTree on " << volSandboxRoot << " failed";
            continue;
        }
        if (android::vold::DeleteDirContentsAndDir(volSandboxRoot) < 0) {
            PLOG(ERROR) << "DeleteDirContentsAndDir failed on " << volSandboxRoot;
            continue;
        }
        LOG(VERBOSE) << "Success: DeleteDirContentsAndDir on " << volSandboxRoot;
    }
    return 0;
}

int VolumeManager::setPrimary(const std::shared_ptr<android::vold::VolumeBase>& vol) {
    if (GetBoolProperty(kIsolatedStorage, false)) {
        return 0;
    }
    mPrimary = vol;
    for (userid_t userId : mStartedUsers) {
        linkPrimary(userId);
    }
    return 0;
}

int VolumeManager::remountUid(uid_t uid, const std::string& mode) {
    // If the isolated storage is enabled, return -1 since in the isolated storage world, there
    // are no longer any runtime storage permissions, so this shouldn't be called anymore.
    if (GetBoolProperty(kIsolatedStorage, false)) {
        return -1;
    }
    LOG(DEBUG) << "Remounting " << uid << " as mode " << mode;

    DIR* dir;
    struct dirent* de;
    std::string rootName;
    std::string pidName;
    int pidFd;
    int nsFd;
    struct stat sb;
    pid_t child;

    if (!(dir = opendir("/proc"))) {
        PLOG(ERROR) << "Failed to opendir";
        return -1;
    }

    // Figure out root namespace to compare against below
    if (!android::vold::Readlinkat(dirfd(dir), "1/ns/mnt", &rootName)) {
        PLOG(ERROR) << "Failed to read root namespace";
        closedir(dir);
        return -1;
    }

    // Poke through all running PIDs look for apps running as UID
    while ((de = readdir(dir))) {
        pid_t pid;
        if (de->d_type != DT_DIR) continue;
        if (!android::base::ParseInt(de->d_name, &pid)) continue;

        pidFd = -1;
        nsFd = -1;

        pidFd = openat(dirfd(dir), de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (pidFd < 0) {
            goto next;
        }
        if (fstat(pidFd, &sb) != 0) {
            PLOG(WARNING) << "Failed to stat " << de->d_name;
            goto next;
        }
        if (sb.st_uid != uid) {
            goto next;
        }

        // Matches so far, but refuse to touch if in root namespace
        LOG(DEBUG) << "Found matching PID " << de->d_name;
        if (!android::vold::Readlinkat(pidFd, "ns/mnt", &pidName)) {
            PLOG(WARNING) << "Failed to read namespace for " << de->d_name;
            goto next;
        }
        if (rootName == pidName) {
            LOG(WARNING) << "Skipping due to root namespace";
            goto next;
        }

        // We purposefully leave the namespace open across the fork
        nsFd = openat(pidFd, "ns/mnt", O_RDONLY);  // not O_CLOEXEC
        if (nsFd < 0) {
            PLOG(WARNING) << "Failed to open namespace for " << de->d_name;
            goto next;
        }

        if (!(child = fork())) {
            if (setns(nsFd, CLONE_NEWNS) != 0) {
                PLOG(ERROR) << "Failed to setns for " << de->d_name;
                _exit(1);
            }

            android::vold::UnmountTree("/storage/");

            std::string storageSource;
            if (mode == "default") {
                storageSource = "/mnt/runtime/default";
            } else if (mode == "read") {
                storageSource = "/mnt/runtime/read";
            } else if (mode == "write") {
                storageSource = "/mnt/runtime/write";
            } else {
                // Sane default of no storage visible
                _exit(0);
            }
            if (TEMP_FAILURE_RETRY(
                    mount(storageSource.c_str(), "/storage", NULL, MS_BIND | MS_REC, NULL)) == -1) {
                PLOG(ERROR) << "Failed to mount " << storageSource << " for " << de->d_name;
                _exit(1);
            }
            if (TEMP_FAILURE_RETRY(mount(NULL, "/storage", NULL, MS_REC | MS_SLAVE, NULL)) == -1) {
                PLOG(ERROR) << "Failed to set MS_SLAVE to /storage for " << de->d_name;
                _exit(1);
            }

            // Mount user-specific symlink helper into place
            userid_t user_id = multiuser_get_user_id(uid);
            std::string userSource(StringPrintf("/mnt/user/%d", user_id));
            if (TEMP_FAILURE_RETRY(
                    mount(userSource.c_str(), "/storage/self", NULL, MS_BIND, NULL)) == -1) {
                PLOG(ERROR) << "Failed to mount " << userSource << " for " << de->d_name;
                _exit(1);
            }

            _exit(0);
        }

        if (child == -1) {
            PLOG(ERROR) << "Failed to fork";
            goto next;
        } else {
            TEMP_FAILURE_RETRY(waitpid(child, nullptr, 0));
        }

    next:
        close(nsFd);
        close(pidFd);
    }
    closedir(dir);
    return 0;
}

int VolumeManager::reset() {
    // Tear down all existing disks/volumes and start from a blank slate so
    // newly connected framework hears all events.
    if (mInternalEmulated != nullptr) {
        mInternalEmulated->destroy();
        mInternalEmulated->create();
    }
    for (const auto& disk : mDisks) {
        disk->destroy();
        disk->create();
    }
    updateVirtualDisk();
    mAddedUsers.clear();
    mStartedUsers.clear();

    mUserPackages.clear();
    mAppIds.clear();
    mSandboxIds.clear();
    mVisibleVolumeIds.clear();

    // For unmounting dirs under /mnt/user/<user-id>/package/<package-name>
    android::vold::UnmountTree("/mnt/user/");
    return 0;
}

// Can be called twice (sequentially) during shutdown. should be safe for that.
int VolumeManager::shutdown() {
    if (mInternalEmulated == nullptr) {
        return 0;  // already shutdown
    }
    android::vold::sSleepOnUnmount = false;
    mInternalEmulated->destroy();
    mInternalEmulated = nullptr;
    for (const auto& disk : mDisks) {
        disk->destroy();
    }
    mDisks.clear();
    mPendingDisks.clear();
    android::vold::sSleepOnUnmount = true;
    return 0;
}

int VolumeManager::unmountAll() {
    std::lock_guard<std::mutex> lock(mLock);
    ATRACE_NAME("VolumeManager::unmountAll()");

    // First, try gracefully unmounting all known devices
    if (mInternalEmulated != nullptr) {
        mInternalEmulated->unmount();
    }
    for (const auto& disk : mDisks) {
        disk->unmountAll();
    }

    // Worst case we might have some stale mounts lurking around, so
    // force unmount those just to be safe.
    FILE* fp = setmntent("/proc/mounts", "r");
    if (fp == NULL) {
        PLOG(ERROR) << "Failed to open /proc/mounts";
        return -errno;
    }

    // Some volumes can be stacked on each other, so force unmount in
    // reverse order to give us the best chance of success.
    std::list<std::string> toUnmount;
    mntent* mentry;
    while ((mentry = getmntent(fp)) != NULL) {
        auto test = std::string(mentry->mnt_dir);
        if ((StartsWith(test, "/mnt/") &&
#ifdef __ANDROID_DEBUGGABLE__
             !StartsWith(test, "/mnt/scratch") &&
#endif
             !StartsWith(test, "/mnt/vendor") && !StartsWith(test, "/mnt/product")) ||
            StartsWith(test, "/storage/")) {
            toUnmount.push_front(test);
        }
    }
    endmntent(fp);

    for (const auto& path : toUnmount) {
        LOG(DEBUG) << "Tearing down stale mount " << path;
        android::vold::ForceUnmount(path);
    }

    return 0;
}

int VolumeManager::mkdirs(const std::string& path) {
    // Only offer to create directories for paths managed by vold
    if (StartsWith(path, "/storage/")) {
        // fs_mkdirs() does symlink checking and relative path enforcement
        return fs_mkdirs(path.c_str(), 0700);
    } else {
        LOG(ERROR) << "Failed to find mounted volume for " << path;
        return -EINVAL;
    }
}

static size_t kAppFuseMaxMountPointName = 32;

static android::status_t getMountPath(uid_t uid, const std::string& name, std::string* path) {
    if (name.size() > kAppFuseMaxMountPointName) {
        LOG(ERROR) << "AppFuse mount name is too long.";
        return -EINVAL;
    }
    for (size_t i = 0; i < name.size(); i++) {
        if (!isalnum(name[i])) {
            LOG(ERROR) << "AppFuse mount name contains invalid character.";
            return -EINVAL;
        }
    }
    *path = StringPrintf("/mnt/appfuse/%d_%s", uid, name.c_str());
    return android::OK;
}

static android::status_t mountInNamespace(uid_t uid, int device_fd, const std::string& path) {
    // Remove existing mount.
    android::vold::ForceUnmount(path);

    const auto opts = StringPrintf(
        "fd=%i,"
        "rootmode=40000,"
        "default_permissions,"
        "allow_other,"
        "user_id=%d,group_id=%d,"
        "context=\"u:object_r:app_fuse_file:s0\","
        "fscontext=u:object_r:app_fusefs:s0",
        device_fd, uid, uid);

    const int result =
        TEMP_FAILURE_RETRY(mount("/dev/fuse", path.c_str(), "fuse",
                                 MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME, opts.c_str()));
    if (result != 0) {
        PLOG(ERROR) << "Failed to mount " << path;
        return -errno;
    }

    return android::OK;
}

static android::status_t runCommandInNamespace(const std::string& command, uid_t uid, pid_t pid,
                                               const std::string& path, int device_fd) {
    if (DEBUG_APPFUSE) {
        LOG(DEBUG) << "Run app fuse command " << command << " for the path " << path
                   << " in namespace " << uid;
    }

    unique_fd dir(open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() == -1) {
        PLOG(ERROR) << "Failed to open /proc";
        return -errno;
    }

    // Obtains process file descriptor.
    const std::string pid_str = StringPrintf("%d", pid);
    const unique_fd pid_fd(openat(dir.get(), pid_str.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (pid_fd.get() == -1) {
        PLOG(ERROR) << "Failed to open /proc/" << pid;
        return -errno;
    }

    // Check UID of process.
    {
        struct stat sb;
        const int result = fstat(pid_fd.get(), &sb);
        if (result == -1) {
            PLOG(ERROR) << "Failed to stat /proc/" << pid;
            return -errno;
        }
        if (sb.st_uid != AID_SYSTEM) {
            LOG(ERROR) << "Only system can mount appfuse. UID expected=" << AID_SYSTEM
                       << ", actual=" << sb.st_uid;
            return -EPERM;
        }
    }

    // Matches so far, but refuse to touch if in root namespace
    {
        std::string rootName;
        std::string pidName;
        if (!android::vold::Readlinkat(dir.get(), "1/ns/mnt", &rootName) ||
            !android::vold::Readlinkat(pid_fd.get(), "ns/mnt", &pidName)) {
            PLOG(ERROR) << "Failed to read namespaces";
            return -EPERM;
        }
        if (rootName == pidName) {
            LOG(ERROR) << "Don't mount appfuse in root namespace";
            return -EPERM;
        }
    }

    // We purposefully leave the namespace open across the fork
    unique_fd ns_fd(openat(pid_fd.get(), "ns/mnt", O_RDONLY));  // not O_CLOEXEC
    if (ns_fd.get() < 0) {
        PLOG(ERROR) << "Failed to open namespace for /proc/" << pid << "/ns/mnt";
        return -errno;
    }

    int child = fork();
    if (child == 0) {
        if (setns(ns_fd.get(), CLONE_NEWNS) != 0) {
            PLOG(ERROR) << "Failed to setns";
            _exit(-errno);
        }

        if (command == "mount") {
            _exit(mountInNamespace(uid, device_fd, path));
        } else if (command == "unmount") {
            // If it's just after all FD opened on mount point are closed, umount2 can fail with
            // EBUSY. To avoid the case, specify MNT_DETACH.
            if (umount2(path.c_str(), UMOUNT_NOFOLLOW | MNT_DETACH) != 0 && errno != EINVAL &&
                errno != ENOENT) {
                PLOG(ERROR) << "Failed to unmount directory.";
                _exit(-errno);
            }
            if (rmdir(path.c_str()) != 0) {
                PLOG(ERROR) << "Failed to remove the mount directory.";
                _exit(-errno);
            }
            _exit(android::OK);
        } else {
            LOG(ERROR) << "Unknown appfuse command " << command;
            _exit(-EPERM);
        }
    }

    if (child == -1) {
        PLOG(ERROR) << "Failed to folk child process";
        return -errno;
    }

    android::status_t status;
    TEMP_FAILURE_RETRY(waitpid(child, &status, 0));

    return status;
}

int VolumeManager::createObb(const std::string& sourcePath, const std::string& sourceKey,
                             int32_t ownerGid, std::string* outVolId) {
    int id = mNextObbId++;

    auto vol = std::shared_ptr<android::vold::VolumeBase>(
        new android::vold::ObbVolume(id, sourcePath, sourceKey, ownerGid));
    vol->create();

    mObbVolumes.push_back(vol);
    *outVolId = vol->getId();
    return android::OK;
}

int VolumeManager::destroyObb(const std::string& volId) {
    auto i = mObbVolumes.begin();
    while (i != mObbVolumes.end()) {
        if ((*i)->getId() == volId) {
            (*i)->destroy();
            i = mObbVolumes.erase(i);
        } else {
            ++i;
        }
    }
    return android::OK;
}

int VolumeManager::mountAppFuse(uid_t uid, pid_t pid, int mountId, unique_fd* device_fd) {
    std::string name = std::to_string(mountId);

    // Check mount point name.
    std::string path;
    if (getMountPath(uid, name, &path) != android::OK) {
        LOG(ERROR) << "Invalid mount point name";
        return -1;
    }

    // Create directories.
    const android::status_t result = android::vold::PrepareDir(path, 0700, 0, 0);
    if (result != android::OK) {
        PLOG(ERROR) << "Failed to prepare directory " << path;
        return -1;
    }

    // Open device FD.
    device_fd->reset(open("/dev/fuse", O_RDWR));  // not O_CLOEXEC
    if (device_fd->get() == -1) {
        PLOG(ERROR) << "Failed to open /dev/fuse";
        return -1;
    }

    // Mount.
    return runCommandInNamespace("mount", uid, pid, path, device_fd->get());
}

int VolumeManager::unmountAppFuse(uid_t uid, pid_t pid, int mountId) {
    std::string name = std::to_string(mountId);

    // Check mount point name.
    std::string path;
    if (getMountPath(uid, name, &path) != android::OK) {
        LOG(ERROR) << "Invalid mount point name";
        return -1;
    }

    return runCommandInNamespace("unmount", uid, pid, path, -1 /* device_fd */);
}
