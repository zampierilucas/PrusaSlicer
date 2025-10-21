///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CloudSyncManager.hpp"
#include "CloudSync.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

#include <iomanip>
#include <sstream>
#include <thread>
#include <sys/stat.h>

namespace fs = boost::filesystem;

namespace Slic3r {

const std::string CloudSyncManager::REMOTE_BUNDLE_PATH = "/PrusaSlicer/config_bundle.ini";

CloudSyncManager::CloudSyncManager()
    : m_app_config(nullptr)
    , m_is_syncing(false)
    , m_auto_sync_enabled(false)
{
}

CloudSyncManager& CloudSyncManager::instance()
{
    static CloudSyncManager inst;
    return inst;
}

bool CloudSyncManager::initialize(AppConfig *config)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_app_config = config;

    if (!config) {
        return false;
    }

    // Check if cloud sync is enabled
    if (!config->get_bool("cloud_sync", "enabled")) {
        return false;
    }

    // Get WebDAV configuration
    std::string url = config->get("cloud_sync", "url");
    std::string username = config->get("cloud_sync", "username");
    std::string password = config->get("cloud_sync", "password");

    // Validate required configuration
    if (url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "CloudSync: WebDAV URL is not configured";
        return false;
    }

    // Create WebDAV client
    m_webdav = std::make_unique<CloudSyncWebDAV>(url, username, password);

    // Get auto-sync setting
    m_auto_sync_enabled = config->get_bool("cloud_sync", "auto_sync");

    BOOST_LOG_TRIVIAL(info) << boost::format("CloudSync: Initialized with WebDAV at %1%") % url;

    return true;
}

bool CloudSyncManager::is_enabled() const
{
    return m_webdav != nullptr;
}

bool CloudSyncManager::create_backup(const std::string &backup_reason, std::string &backup_path)
{
    try {
        // Create backup directory with timestamp
        time_t now = std::time(nullptr);
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", std::localtime(&now));

        boost::filesystem::path data_path = boost::filesystem::path(data_dir());
        boost::filesystem::path backup_dir = data_path / "backups" / ("backup-" + std::string(timestamp));

        BOOST_LOG_TRIVIAL(info) << boost::format("CloudSync: Creating backup at %1%") % backup_dir.string();

        boost::filesystem::create_directories(backup_dir);

        // Generate config bundle for backup
        PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
        if (preset_bundle) {
            boost::filesystem::path bundle_file = backup_dir / "config_bundle.ini";

            try {
                preset_bundle->export_configbundle(
                    bundle_file.string(),
                    false,  // Don't export system presets
                    true,   // Include physical printers
                    nullptr
                );

                if (boost::filesystem::exists(bundle_file)) {
                    BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Bundle backup created: %1% bytes")
                        % boost::filesystem::file_size(bundle_file);
                }
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("CloudSync: Failed to generate bundle backup: %1%") % e.what();
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "CloudSync: PresetBundle not available for backup";
        }

        // Create simple README
        boost::nowide::ofstream readme((backup_dir / "README.txt").string());
        if (readme.is_open()) {
            readme << "PrusaSlicer Configuration Backup\n"
                   << "Created: " << timestamp << "\n"
                   << "Reason: " << backup_reason << "\n\n"
                   << "To restore: File > Import > Import config bundle\n";
            readme.close();
        }

        backup_path = backup_dir.string();
        BOOST_LOG_TRIVIAL(info) << boost::format("CloudSync: Backup created at %1%") % backup_path;

        return true;

    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: Failed to create backup: %1%") % e.what();
        return false;
    }
}

CloudSyncManager::FirstSyncInfo CloudSyncManager::check_first_sync()
{
    FirstSyncInfo info;

    // Check bootstrap flag first
    if (has_synced_before()) {
        info.state = NOT_FIRST_SYNC;
        return info;
    }

    // Get stored last local modification time
    // This is updated when bundle is generated or imported
    info.local_last_modified = get_last_local_modification_time();
    info.local_exists = (info.local_last_modified > 0);

    // Check remote bundle file
    CloudFile remote_file;
    info.remote_exists = get_remote_bundle_info(remote_file);

    if (info.remote_exists) {
        info.remote_last_modified = remote_file.modified_time;
    }

    // Determine state based on existence
    if (!info.local_exists && !info.remote_exists) {
        info.state = FIRST_SYNC_BOTH_EMPTY;
    } else if (info.local_exists && !info.remote_exists) {
        info.state = FIRST_SYNC_LOCAL_ONLY;
    } else if (!info.local_exists && info.remote_exists) {
        info.state = FIRST_SYNC_REMOTE_ONLY;
    } else {
        info.state = FIRST_SYNC_BOTH_EXIST;
    }

    return info;
}

CloudSyncManager::SyncFileResult CloudSyncManager::sync()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    SyncFileResult result;
    boost::filesystem::path temp_upload_bundle;   // For cleanup
    boost::filesystem::path temp_download_bundle; // For cleanup

    if (!m_webdav) {
        result.success = false;
        result.error_msg = "Cloud sync WebDAV client not initialized";
        return result;
    }

    if (m_is_syncing) {
        result.success = false;
        result.error_msg = "Sync already in progress";
        return result;
    }

    m_is_syncing = true;

    // Check if this is first sync - create backup if both local and remote exist
    if (!has_synced_before()) {
        BOOST_LOG_TRIVIAL(info) << "CloudSync: First sync detected";

        FirstSyncInfo info = check_first_sync();
        if (info.state == FIRST_SYNC_BOTH_EXIST) {
            std::string backup_path;
            if (!create_backup("First sync with conflict resolution", backup_path)) {
                BOOST_LOG_TRIVIAL(warning) << "CloudSync: Failed to create backup";
            }
        }
    }

    // Push "sync started" notification
    push_notification(static_cast<int>(GUI::NotificationType::CloudSyncStarted));

    FileAction action = SKIP;  // Default to SKIP

    try {
        // Ensure remote directory structure exists
        if (!ensure_remote_structure()) {
            result.success = false;
            result.error_msg = "Failed to create remote directory structure";
            m_is_syncing = false;
            return result;
        }

        // Generate fresh bundle from current presets
        boost::filesystem::path local_bundle_path;
        std::string gen_error;
        bool exists_local = generate_bundle_for_sync(local_bundle_path, gen_error);

        if (!exists_local) {
            result.success = false;
            result.error_msg = "Failed to generate config bundle: " + gen_error;
            m_is_syncing = false;
            BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: %1%") % result.error_msg;
            return result;
        }

        temp_upload_bundle = local_bundle_path;  // Mark for cleanup

        // Use stored last_local_modification time (not the generated bundle's current timestamp)
        time_t local_mtime = get_last_local_modification_time();
        std::string local_hash = calculate_file_hash(local_bundle_path);

        // Get remote bundle info (includes hash from WebDAV)
        CloudFile remote_file;
        bool exists_remote = get_remote_bundle_info(remote_file);
        time_t remote_mtime = exists_remote ? remote_file.modified_time : 0;

        // Determine what action to take using smart conflict detection
        action = determine_action(exists_local, exists_remote, local_mtime, remote_mtime,
                                 local_hash, remote_file.hash);

        // Execute action
        std::string remote_path = REMOTE_BUNDLE_PATH;

        switch (action) {
            case UPLOAD: {
                // Delete existing remote file first to ensure clean overwrite
                // (some WebDAV servers create versioned copies instead of overwriting)
                if (exists_remote) {
                    if (!m_webdav->delete_file(remote_path, [](wxString err) {
                        BOOST_LOG_TRIVIAL(warning) << boost::format("CloudSync: Failed to delete existing file: %1%")
                            % err.ToStdString();
                    })) {
                        BOOST_LOG_TRIVIAL(warning) << "CloudSync: Could not delete existing remote file, upload may create versioned copy";
                        // Don't fail - proceed with upload anyway
                    }
                }

                BOOST_LOG_TRIVIAL(info) << "CloudSync: Uploading config bundle";
                if (upload_file(local_bundle_path, remote_path)) {
                    result.was_uploaded = true;

                    // Update last_local_modification timestamp after successful upload
                    if (m_app_config) {
                        time_t upload_time = std::time(nullptr);
                        m_app_config->set("cloud_sync", "last_local_modification", std::to_string(upload_time));
                        BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Updated last_local_modification after upload: %1%") % upload_time;
                    }
                } else {
                    result.success = false;
                    result.error_msg = "Failed to upload config bundle";
                    BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: %1%") % result.error_msg;
                }
                break;
            }

            case DOWNLOAD: {
                // Download to temporary location
                boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path() / "prusaslicer_cloudsync";
                boost::filesystem::create_directories(temp_dir);
                temp_download_bundle = temp_dir / "downloaded_bundle.ini";

                BOOST_LOG_TRIVIAL(info) << "CloudSync: Downloading config bundle";
                if (download_file(remote_file.path, temp_download_bundle)) {
                    // Import the downloaded bundle with remote modification time
                    std::string import_error;
                    if (import_bundle_from_sync(temp_download_bundle, import_error, remote_mtime)) {
                        result.was_uploaded = false;
                    } else {
                        result.success = false;
                        result.error_msg = "Failed to import downloaded bundle: " + import_error;
                        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: %1%") % result.error_msg;
                    }
                } else {
                    result.success = false;
                    result.error_msg = "Failed to download config bundle";
                    BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: %1%") % result.error_msg;
                }
                break;
            }

            case SKIP:
                break;
        }

    } catch (const std::exception &e) {
        result.success = false;
        result.error_msg = std::string("Exception during sync: ") + e.what();
    }

    // Cleanup temporary files
    if (!temp_upload_bundle.empty() && boost::filesystem::exists(temp_upload_bundle)) {
        boost::filesystem::remove(temp_upload_bundle);
    }
    if (!temp_download_bundle.empty() && boost::filesystem::exists(temp_download_bundle)) {
        boost::filesystem::remove(temp_download_bundle);
    }

    m_is_syncing = false;

    // Log final result
    if (result.success) {
        if (action == SKIP) {
            BOOST_LOG_TRIVIAL(info) << "CloudSync: Sync complete - no changes needed";
        } else if (result.was_uploaded) {
            BOOST_LOG_TRIVIAL(info) << "CloudSync: Sync complete - config bundle uploaded";
        } else {
            BOOST_LOG_TRIVIAL(info) << "CloudSync: Sync complete - config bundle downloaded";
        }
    } else {
        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: Sync failed: %1%") % result.error_msg;
    }

    // Push appropriate notification
    if (result.success) {
        // Successful sync
        std::ostringstream msg;
        if (action == SKIP) {
            msg << "CloudSync: Everything up to date";
        } else if (result.was_uploaded) {
            msg << "CloudSync: Config bundle uploaded to cloud";
        } else {
            msg << "CloudSync: Config bundle downloaded from cloud";
        }

        // Mark first sync as complete on first successful sync
        if (!has_synced_before()) {
            mark_first_sync_complete();
        }

        push_notification(static_cast<int>(GUI::NotificationType::CloudSyncCompleted), msg.str());
    } else {
        // Sync failed
        std::string error_msg = "Cloud sync failed";
        if (!result.error_msg.empty()) {
            error_msg += ": " + result.error_msg;
        }
        push_notification(static_cast<int>(GUI::NotificationType::CloudSyncError), error_msg);
    }

    return result;
}

void CloudSyncManager::set_auto_sync_enabled(bool enabled)
{
    m_auto_sync_enabled = enabled;

    if (m_app_config) {
        m_app_config->set("cloud_sync", "auto_sync", enabled ? "1" : "0");
    }
}

void CloudSyncManager::trigger_auto_sync()
{
    // Re-read auto-sync setting from config in case it changed
    if (m_app_config)
        m_auto_sync_enabled = m_app_config->get_bool("cloud_sync", "auto_sync");

    if (!m_auto_sync_enabled || m_is_syncing || !is_enabled())
        return;

    BOOST_LOG_TRIVIAL(debug) << "CloudSync: Triggering auto-sync in background";
    std::thread([this]() {
        this->sync();
    }).detach();
}

// Helper methods

bool CloudSyncManager::ensure_remote_structure()
{
    if (!m_webdav) {
        return false;
    }

    // Create base directory only
    wxString error;
    if (!m_webdav->create_directory("/PrusaSlicer", [&](wxString err) { error = err; })) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Base directory may already exist: %1%")
            % error.ToStdString();
    }

    return true;
}

bool CloudSyncManager::get_local_bundle_path(boost::filesystem::path &local_path)
{
    boost::filesystem::path data_path = boost::filesystem::path(data_dir());
    local_path = data_path / "PrusaSlicer.ini";
    return boost::filesystem::exists(local_path);
}

bool CloudSyncManager::get_remote_bundle_info(CloudFile &remote_file)
{
    if (!m_webdav) {
        return false;
    }

    // Use list_files to get info about the bundle file
    std::vector<CloudFile> files;
    if (!m_webdav->list_files("/PrusaSlicer/", files, [](wxString err) {
        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: Failed to list remote files: %1%")
            % err.ToStdString();
    })) {
        return false;
    }

    // Find config_bundle.ini in the list
    for (const auto &file : files) {
        if (file.name == "config_bundle.ini" && !file.is_directory) {
            remote_file = file;
            return true;
        }
    }

    return false;
}

CloudSyncManager::FileAction CloudSyncManager::determine_action(
    bool exists_local,
    bool exists_remote,
    time_t local_mtime,
    time_t remote_mtime,
    const std::string &local_hash,
    const std::string &remote_hash)
{
    BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: determine_action called - exists_local=%1%, exists_remote=%2%, local_mtime=%3%, remote_mtime=%4%, local_hash=%5%, remote_hash=%6%")
        % exists_local % exists_remote % local_mtime % remote_mtime
        % (local_hash.empty() ? "<empty>" : local_hash.substr(0, 8) + "...")
        % (remote_hash.empty() ? "<empty>" : remote_hash.substr(0, 8) + "...");

    // Case 1: Only local exists
    if (exists_local && !exists_remote) {
        BOOST_LOG_TRIVIAL(debug) << "CloudSync: Action=UPLOAD - Reason: Only local file exists, no remote file found";
        return UPLOAD;
    }

    // Case 2: Only remote exists
    if (!exists_local && exists_remote) {
        BOOST_LOG_TRIVIAL(debug) << "CloudSync: Action=DOWNLOAD - Reason: Only remote file exists, no local file found";
        return DOWNLOAD;
    }

    // Case 3: Both exist - compare hashes first, then timestamps
    if (exists_local && exists_remote) {
        // Compare hashes if both are available
        if (!local_hash.empty() && !remote_hash.empty()) {
            if (local_hash == remote_hash) {
                BOOST_LOG_TRIVIAL(debug) << "CloudSync: Action=SKIP - Reason: Both files exist and hashes match (files are identical)";
                return SKIP;
            }
            BOOST_LOG_TRIVIAL(debug) << "CloudSync: Hashes differ, comparing timestamps for conflict resolution";
        } else {
            BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Hash comparison skipped (local_hash=%1%, remote_hash=%2%), using timestamp comparison")
                % (local_hash.empty() ? "empty" : "present")
                % (remote_hash.empty() ? "empty" : "present");
        }

        // Use timestamp as tiebreaker
        if (local_mtime > remote_mtime) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Action=UPLOAD - Reason: Local file is newer (local_mtime=%1% > remote_mtime=%2%, delta=%3%s)")
                % local_mtime % remote_mtime % (local_mtime - remote_mtime);
            return UPLOAD;
        } else if (remote_mtime > local_mtime) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Action=DOWNLOAD - Reason: Remote file is newer (remote_mtime=%1% > local_mtime=%2%, delta=%3%s)")
                % remote_mtime % local_mtime % (remote_mtime - local_mtime);
            return DOWNLOAD;
        }

        // Timestamps are equal
        BOOST_LOG_TRIVIAL(debug) << "CloudSync: Action=SKIP - Reason: Both files exist with identical timestamps (local_mtime == remote_mtime)";
    }

    BOOST_LOG_TRIVIAL(debug) << "CloudSync: Action=SKIP - Reason: Default fallback (neither local nor remote exists, or unexpected state)";
    return SKIP;
}

bool CloudSyncManager::upload_file(const boost::filesystem::path &local_path, const std::string &remote_path)
{
    if (!m_webdav) {
        return false;
    }

    bool success = m_webdav->upload_file(local_path, remote_path,
        [](Http::Progress progress, bool &cancel) {
            // Progress callback
        },
        [](wxString error) {
            BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: Upload failed: %1%") % error.ToStdString();
        });

    return success;
}

bool CloudSyncManager::download_file(const std::string &remote_path, const boost::filesystem::path &local_path)
{
    if (!m_webdav) {
        return false;
    }

    // Ensure local directory exists
    boost::filesystem::create_directories(local_path.parent_path());

    bool success = m_webdav->download_file(remote_path, local_path,
        [](Http::Progress progress, bool &cancel) {
            // Progress callback
        },
        [](wxString error) {
            BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: Download failed: %1%") % error.ToStdString();
        });

    return success;
}

std::string CloudSyncManager::calculate_file_hash(const boost::filesystem::path &path)
{
    // Read entire file into memory for hash calculation
    boost::nowide::ifstream file(path.string(), std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    // Read file content
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Use shared hash function that skips timestamp header
    return content.empty() ? "" : calculate_config_bundle_hash(content.c_str(), content.size());
}

time_t CloudSyncManager::get_last_local_modification_time()
{
    if (!m_app_config) {
        return 0;
    }

    // Get stored last_modified from config
    std::string stored_time = m_app_config->get("cloud_sync", "last_local_modification");

    if (!stored_time.empty()) {
        try {
            time_t timestamp = std::stoll(stored_time);
            BOOST_LOG_TRIVIAL(trace) << boost::format("CloudSync: Retrieved last_local_modification: %1%") % timestamp;
            return timestamp;
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("CloudSync: Invalid last_local_modification value: %1%") % stored_time;
        }
    }

    // If not stored or invalid, return 0 to indicate unknown/never modified
    // This will cause the sync logic to prefer remote if it exists
    BOOST_LOG_TRIVIAL(trace) << "CloudSync: No valid last_local_modification found, returning 0";
    return 0;
}

// First sync marker management - minimal, decoupled from sync decision logic
bool CloudSyncManager::has_synced_before() const
{
    if (!m_app_config) {
        return false;
    }
    return m_app_config->get_bool("cloud_sync", "bootstrapped");
}

void CloudSyncManager::mark_first_sync_complete()
{
    if (!m_app_config) {
        BOOST_LOG_TRIVIAL(warning) << "CloudSync: AppConfig not available, cannot mark first sync complete";
        return;
    }
    m_app_config->set("cloud_sync", "bootstrapped", "1");
    // Don't call save() here - called from worker thread, will be saved by GUI from main thread
    BOOST_LOG_TRIVIAL(info) << "CloudSync: Marked first sync as complete";
}

void CloudSyncManager::push_notification(int type, const std::string &message)
{
    try {
        auto* plater = GUI::wxGetApp().plater();
        if (plater && plater->get_notification_manager()) {
            auto notification_type = static_cast<GUI::NotificationType>(type);
            if (message.empty()) {
                plater->get_notification_manager()->push_notification(notification_type);
            } else {
                plater->get_notification_manager()->push_notification(
                    notification_type,
                    GUI::NotificationManager::NotificationLevel::RegularNotificationLevel,
                    message
                );
            }
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: Failed to push notification: %1%") % e.what();
    }
}

bool CloudSyncManager::generate_bundle_for_sync(
    boost::filesystem::path &bundle_path,
    std::string &error_msg)
{
    // Access PresetBundle directly from the application
    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;

    if (!preset_bundle) {
        error_msg = "PresetBundle not available";
        return false;
    }

    try {
        // Create temp directory for bundle generation
        boost::filesystem::path temp_dir = boost::filesystem::temp_directory_path() / "prusaslicer_cloudsync";
        boost::filesystem::create_directories(temp_dir);

        // Generate unique filename with timestamp
        time_t now = std::time(nullptr);
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", std::localtime(&now));

        bundle_path = temp_dir / (std::string("sync_bundle_") + timestamp + ".ini");

        // Use PresetBundle's export_configbundle method
        preset_bundle->export_configbundle(
            bundle_path.string(),
            false,  // export_system_settings = false (don't include built-in presets)
            true,   // export_physical_printers = true
            nullptr // No secret callback needed
        );

        if (!boost::filesystem::exists(bundle_path)) {
            error_msg = "Bundle generation failed - file not created";
            return false;
        }

        // NOTE: Do NOT update last_local_modification here!
        // This function is called to generate a bundle for comparison/sync,
        // not because the user modified presets. The timestamp should only
        // be updated when:
        // 1. User explicitly modifies presets (handled elsewhere)
        // 2. We successfully upload (handled in upload_file success callback)
        // 3. We successfully import from remote (handled in import_bundle_from_sync)

        return true;

    } catch (const std::exception &e) {
        error_msg = std::string("Exception during bundle generation: ") + e.what();
        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: %1%") % error_msg;
        return false;
    }
}

bool CloudSyncManager::import_bundle_from_sync(
    const boost::filesystem::path &bundle_path,
    std::string &error_msg,
    time_t remote_mtime)
{
    // Access PresetBundle directly from the application
    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;

    if (!preset_bundle) {
        error_msg = "PresetBundle not available";
        return false;
    }

    if (!boost::filesystem::exists(bundle_path)) {
        error_msg = "Bundle file does not exist: " + bundle_path.string();
        return false;
    }

    try {
        // Use PresetBundle's load_configbundle method
        auto [substitutions, presets_imported] = preset_bundle->load_configbundle(
            bundle_path.string(),
            PresetBundle::SaveImported,  // Save the imported presets to user directory
            ForwardCompatibilitySubstitutionRule::Enable
        );

        // Log substitutions if any
        if (!substitutions.empty()) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("CloudSync: Config substitutions during import: %1% items")
                % substitutions.size();
        }

        // Update last_local_modification to match the remote file's modification time
        // This ensures our local timestamp reflects when the remote was last modified,
        // not when we downloaded it
        if (m_app_config) {
            time_t timestamp_to_use = (remote_mtime > 0) ? remote_mtime : std::time(nullptr);
            m_app_config->set("cloud_sync", "last_local_modification", std::to_string(timestamp_to_use));
            BOOST_LOG_TRIVIAL(debug) << boost::format("CloudSync: Updated last_local_modification after import: %1% (remote_mtime=%2%)")
                % timestamp_to_use % remote_mtime;
        }

        return true;

    } catch (const std::exception &e) {
        error_msg = std::string("Exception during bundle import: ") + e.what();
        BOOST_LOG_TRIVIAL(error) << boost::format("CloudSync: %1%") % error_msg;
        return false;
    }
}

} // namespace Slic3r
