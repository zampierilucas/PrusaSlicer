///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CloudSyncManager.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

#include <openssl/md5.h>
#include <iomanip>
#include <sstream>
#include <thread>

namespace Slic3r {

const std::string CloudSyncManager::DELETION_LOG_FILE = "cloud_deletion_log.json";
const std::string CloudSyncManager::REMOTE_BASE_PATH = "/PrusaSlicer";
const time_t CloudSyncManager::DELETION_EXPIRY_DAYS = 30 * 24 * 60 * 60; // 30 days in seconds

const std::vector<std::string>& CloudSyncManager::get_preset_types()
{
    static const std::vector<std::string> preset_types = {"print", "filament", "sla_print", "sla_material", "printer"};
    return preset_types;
}

bool CloudSyncManager::is_valid_preset_type(const std::string &type)
{
    const auto &types = get_preset_types();
    return std::find(types.begin(), types.end(), type) != types.end();
}

CloudSyncManager::CloudSyncManager()
    : m_app_config(nullptr)
    , m_is_syncing(false)
    , m_auto_sync_enabled(false)
    , m_last_sync_time(0)
    , m_last_cleanup_time(0)
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

    // Create provider from config
    m_provider = CloudSyncProvider::create_from_config(*config);

    if (!m_provider) {
        BOOST_LOG_TRIVIAL(error) << "CloudSyncManager: Failed to create cloud sync provider";
        return false;
    }

    // Load deletion log
    load_deletion_log();

    // Get auto-sync setting
    m_auto_sync_enabled = config->get_bool("cloud_sync", "auto_sync");

    return true;
}

bool CloudSyncManager::is_enabled() const
{
    return m_provider != nullptr;
}

bool CloudSyncManager::test_connection(wxString &error_msg)
{
    if (!m_provider) {
        error_msg = wxString::FromUTF8("Cloud sync provider not initialized");
        return false;
    }

    return m_provider->test_connection(error_msg);
}

CloudSyncManager::FirstSyncInfo CloudSyncManager::check_first_sync()
{
    FirstSyncInfo info;

    if (!m_provider) {
        info.state = NOT_FIRST_SYNC;
        return info;
    }

    // Check if we've synced before by looking for last_sync_time
    if (m_last_sync_time > 0) {
        info.state = NOT_FIRST_SYNC;
        return info;
    }

    // Also check if sync metadata file exists
    boost::filesystem::path metadata_path = boost::filesystem::path(data_dir()) / DELETION_LOG_FILE;
    if (boost::filesystem::exists(metadata_path)) {
        info.state = NOT_FIRST_SYNC;
        return info;
    }

    // This is first sync - scan local and remote to determine state
    std::vector<std::pair<std::string, boost::filesystem::path>> local_files;
    scan_local_files(local_files);

    // Filter to only .ini files
    info.local_file_count = 0;
    info.local_last_modified = 0;
    for (const auto &file : local_files) {
        if (file.second.extension() == ".ini") {
            info.local_file_count++;
            time_t mtime = boost::filesystem::last_write_time(file.second);
            if (mtime > info.local_last_modified) {
                info.local_last_modified = mtime;
            }
        }
    }

    std::vector<CloudFile> remote_files;
    scan_remote_files(remote_files);

    info.remote_file_count = 0;
    info.remote_last_modified = 0;
    for (const auto &file : remote_files) {
        if (!file.is_directory) {
            info.remote_file_count++;
            if (file.modified_time > info.remote_last_modified) {
                info.remote_last_modified = file.modified_time;
            }
        }
    }

    // Determine state based on counts
    if (info.local_file_count == 0 && info.remote_file_count == 0) {
        info.state = FIRST_SYNC_BOTH_EMPTY;
    } else if (info.local_file_count > 0 && info.remote_file_count == 0) {
        info.state = FIRST_SYNC_LOCAL_ONLY;
    } else if (info.local_file_count == 0 && info.remote_file_count > 0) {
        info.state = FIRST_SYNC_REMOTE_ONLY;
    } else {
        info.state = FIRST_SYNC_BOTH_EXIST;
    }

    BOOST_LOG_TRIVIAL(info) << "First sync check: local=" << info.local_file_count
                           << " remote=" << info.remote_file_count
                           << " state=" << info.state;

    return info;
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

        BOOST_LOG_TRIVIAL(info) << "Creating backup: " << backup_dir.string();

        // Create backup directory
        boost::filesystem::create_directories(backup_dir);

        // Copy PrusaSlicer.ini if it exists
        boost::filesystem::path config_file = data_path / "PrusaSlicer.ini";
        if (boost::filesystem::exists(config_file)) {
            boost::filesystem::copy_file(config_file, backup_dir / "PrusaSlicer.ini",
                                        boost::filesystem::copy_option::overwrite_if_exists);
        }

        // Copy all preset files
        boost::filesystem::path presets_dir;
        boost::filesystem::path with_presets_subdir = data_path / "presets";
        if (boost::filesystem::exists(with_presets_subdir) && boost::filesystem::is_directory(with_presets_subdir)) {
            presets_dir = with_presets_subdir;
        } else {
            presets_dir = data_path;
        }

        size_t files_backed_up = 0;

        for (const auto &type : get_preset_types()) {
            boost::filesystem::path type_dir = presets_dir / type;

            if (!boost::filesystem::exists(type_dir)) {
                continue;
            }

            boost::filesystem::path backup_type_dir = backup_dir / "presets" / type;
            boost::filesystem::create_directories(backup_type_dir);

            for (boost::filesystem::directory_iterator it(type_dir); it != boost::filesystem::directory_iterator(); ++it) {
                if (boost::filesystem::is_regular_file(it->path()) && it->path().extension() == ".ini") {
                    boost::filesystem::copy_file(it->path(),
                                                backup_type_dir / it->path().filename(),
                                                boost::filesystem::copy_option::overwrite_if_exists);
                    files_backed_up++;
                }
            }
        }

        // Create a README file in the backup
        boost::filesystem::path readme_path = backup_dir / "README.txt";
        boost::nowide::ofstream readme(readme_path.string());
        if (readme.is_open()) {
            readme << "PrusaSlicer Configuration Backup\n";
            readme << "Created: " << timestamp << "\n";
            readme << "Reason: " << backup_reason << "\n";
            readme << "Files backed up: " << files_backed_up << "\n";
            readme << "\n";
            readme << "To restore this backup, copy the contents back to your PrusaSlicer data directory.\n";
            readme.close();
        }

        backup_path = backup_dir.string();

        BOOST_LOG_TRIVIAL(info) << "Backup created successfully: " << files_backed_up
                               << " files backed up to " << backup_path;

        return true;

    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to create backup: " << e.what();
        return false;
    }
}

CloudSyncManager::SyncResult CloudSyncManager::sync(SyncMode mode, SyncProgressFn progress_fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    SyncResult result;

    if (!m_provider) {
        result.success = false;
        result.errors.push_back("Cloud sync provider not initialized");
        return result;
    }

    if (m_is_syncing) {
        result.success = false;
        result.errors.push_back("Sync already in progress");
        return result;
    }

    m_is_syncing = true;

    // Push "sync started" notification
    push_notification(static_cast<int>(GUI::NotificationType::CloudSyncStarted));

    try {
        if (progress_fn) {
            progress_fn("Starting cloud sync...", 0);
        }

        // Ensure remote directory structure exists
        if (!ensure_remote_structure()) {
            result.success = false;
            result.errors.push_back("Failed to create remote directory structure");
            m_is_syncing = false;
            return result;
        }

        // Scan local files
        std::vector<std::pair<std::string, boost::filesystem::path>> local_files;
        if (!scan_local_files(local_files)) {
            result.success = false;
            result.errors.push_back("Failed to scan local files");
            m_is_syncing = false;
            return result;
        }

        if (progress_fn) {
            progress_fn("Scanning remote files...", 10);
        }

        // Scan remote files
        std::vector<CloudFile> remote_files;
        if (!scan_remote_files(remote_files)) {
            result.success = false;
            result.errors.push_back("Failed to scan remote files");
            m_is_syncing = false;
            return result;
        }

        if (progress_fn) {
            progress_fn("Analyzing changes...", 20);
        }

        // Build map of remote files for quick lookup
        std::map<std::string, CloudFile> remote_file_map;
        for (const auto &rf : remote_files) {
            remote_file_map[rf.path] = rf;
        }

        // Build map of local files for quick lookup (by remote path)
        std::map<std::string, boost::filesystem::path> local_file_map;
        for (const auto &lf : local_files) {
            std::string remote_path = get_remote_path(lf.first, lf.second.filename().string());
            local_file_map[remote_path] = lf.second;
        }

        // Cleanup old deletion log entries (>30 days)
        time_t now = std::time(nullptr);
        if (now - m_last_cleanup_time > 86400) { // Clean once per day
            cleanup_old_deletions();
            m_last_cleanup_time = now;
        }

        size_t total_operations = local_files.size() + remote_files.size();
        size_t current_operation = 0;

        // Process deletions: files in deletion log that still exist remotely
        if (mode != SYNC_PULL) {
            std::vector<std::string> deletions_processed;

            for (const auto &deleted : m_deleted_files) {
                auto remote_it = remote_file_map.find(deleted.remote_path);
                if (remote_it != remote_file_map.end()) {
                    // File is marked deleted but still exists remotely - delete it
                    if (progress_fn) {
                        int percent = 20 + (20 * ++current_operation / total_operations);
                        progress_fn("Deleting remote file...", percent);
                    }

                    BOOST_LOG_TRIVIAL(info) << "Deleting remote file from deletion log: " << deleted.remote_path;
                    if (m_provider->delete_file(deleted.remote_path, [](wxString err) {
                        BOOST_LOG_TRIVIAL(error) << "Failed to delete remote file: " << err.ToStdString();
                    })) {
                        // Successfully deleted, remove from remote_file_map
                        remote_file_map.erase(remote_it);
                        deletions_processed.push_back(deleted.remote_path);
                    }
                }
            }

            // Remove successfully processed deletions from log
            for (const auto &path : deletions_processed) {
                auto it = m_deleted_files.begin();
                while (it != m_deleted_files.end()) {
                    if (it->remote_path == path) {
                        it = m_deleted_files.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

        // Process all files (local and remote)
        std::set<std::string> processed_paths;

        // Process local files
        for (const auto &local_file : local_files) {
            const std::string &preset_type = local_file.first;
            const boost::filesystem::path &local_path = local_file.second;

            std::string filename = local_path.filename().string();
            std::string remote_path = get_remote_path(preset_type, filename);

            auto remote_it = remote_file_map.find(remote_path);
            bool exists_remote = (remote_it != remote_file_map.end());

            // Use empty CloudFile if remote doesn't exist
            CloudFile remote_file;
            if (exists_remote) {
                remote_file = remote_it->second;
            }

            // Determine what action to take
            FileAction action = determine_action(local_path, remote_file, true, exists_remote);

            // Execute action based on mode
            switch (action) {
                case UPLOAD:
                    if (mode != SYNC_PULL) {
                        if (progress_fn) {
                            int percent = 20 + (70 * ++current_operation / total_operations);
                            progress_fn("Uploading " + filename + "...", percent);
                        }

                        if (upload_file(local_path, remote_path)) {
                            result.files_uploaded++;
                        } else {
                            result.errors.push_back("Failed to upload: " + filename);
                        }
                    }
                    break;

                case DOWNLOAD:
                    if (mode != SYNC_PUSH) {
                        if (progress_fn) {
                            int percent = 20 + (70 * ++current_operation / total_operations);
                            progress_fn("Downloading " + filename + "...", percent);
                        }

                        if (download_file(remote_path, local_path)) {
                            result.files_downloaded++;
                        } else {
                            result.errors.push_back("Failed to download: " + filename);
                        }
                    }
                    break;

                case DELETE_REMOTE:
                    if (mode != SYNC_PULL) {
                        if (progress_fn) {
                            int percent = 20 + (70 * ++current_operation / total_operations);
                            progress_fn("Deleting remote file...", percent);
                        }

                        BOOST_LOG_TRIVIAL(info) << "Deleting remote file: " + remote_path;
                        m_provider->delete_file(remote_path, [](wxString err) {
                            BOOST_LOG_TRIVIAL(error) << "Failed to delete remote file: " << err.ToStdString();
                        });
                    }
                    break;

                case SKIP:
                    // No action needed
                    break;
            }

            processed_paths.insert(remote_path);
        }

        // Process remote files not present locally
        for (const auto &remote_entry : remote_file_map) {
            const CloudFile &remote_file = remote_entry.second;

            if (remote_file.is_directory) {
                continue;
            }

            // Skip if already processed
            if (processed_paths.find(remote_file.path) != processed_paths.end()) {
                continue;
            }

            // Extract filename and check if it's a temporary file
            std::string filename = remote_file.name;

            // Filter out temporary files (pattern: name-timestamp-random.ini)
            // Example: "Vyper%20-%20ABS.ini-1760536505.840984-cAS8Esas.ini"
            bool is_temp_file = false;
            if (filename.find("-") != std::string::npos) {
                // Check if filename has pattern: .ini-<digits>.<digits>-<random>.ini
                size_t second_dash = filename.rfind("-");
                if (second_dash != std::string::npos) {
                    size_t first_dash = filename.rfind("-", second_dash - 1);
                    if (first_dash != std::string::npos) {
                        std::string middle_part = filename.substr(first_dash + 1, second_dash - first_dash - 1);
                        // Check if middle part looks like a timestamp (contains digits and dots)
                        bool has_digit = false;
                        bool has_dot = false;
                        for (char c : middle_part) {
                            if (std::isdigit(c)) has_digit = true;
                            if (c == '.') has_dot = true;
                        }
                        if (has_digit && has_dot && middle_part.length() > 10) {
                            is_temp_file = true;
                        }
                    }
                }
            }

            if (is_temp_file) {
                BOOST_LOG_TRIVIAL(debug) << "Skipping remote temporary file: " << filename;
                std::cerr << "[Sync] Skipping remote temporary file: " << filename << std::endl;
                // In push mode, we should delete these temp files from remote
                if (mode == SYNC_PUSH) {
                    BOOST_LOG_TRIVIAL(info) << "Deleting remote temporary file: " << remote_file.path;
                    m_provider->delete_file(remote_file.path, [](wxString err) {
                        BOOST_LOG_TRIVIAL(error) << "Failed to delete remote temp file: " << err.ToStdString();
                    });
                }
                continue;
            }

            if (mode == SYNC_PUSH) {
                // In push-only mode, track deletion of missing local files
                add_to_deletion_log(remote_file.path);
                continue;
            }
            std::string preset_type = "print"; // Default fallback
            std::string path = remote_file.path;

            // Remove leading slash if present
            if (!path.empty() && path[0] == '/') {
                path = path.substr(1);
            }

            // Parse preset type from path structure
            std::vector<std::string> path_parts;
            boost::split(path_parts, path, boost::is_any_of("/"));

            if (path_parts.size() >= 2) {
                size_t type_index = 0;
                if (path_parts[0] == "PrusaSlicer") {
                    type_index = 1;
                }

                std::string candidate_type = path_parts[type_index];
                if (is_valid_preset_type(candidate_type)) {
                    preset_type = candidate_type;
                }
            }

            boost::filesystem::path local_path = boost::filesystem::path(data_dir()) / "presets" / preset_type / filename;

            if (progress_fn) {
                int percent = 20 + (70 * ++current_operation / total_operations);
                progress_fn("Downloading " + filename + "...", percent);
            }

            if (download_file(remote_file.path, local_path)) {
                result.files_downloaded++;
            } else {
                result.errors.push_back("Failed to download: " + filename);
            }
        }

        if (progress_fn) {
            progress_fn("Saving deletion log...", 95);
        }

        // Save deletion log and update last sync time
        m_last_sync_time = std::time(nullptr);
        save_deletion_log();

        // Note: AppConfig::save() cannot be called from worker thread
        // The last_sync_time is saved in metadata file, which is sufficient
        // If needed, the timestamp can be updated in AppConfig during the next
        // main-thread operation (e.g., when user opens preferences)

        if (progress_fn) {
            progress_fn("Sync complete!", 100);
        }

    } catch (const std::exception &e) {
        result.success = false;
        result.errors.push_back(std::string("Exception during sync: ") + e.what());
    }

    m_is_syncing = false;

    // Determine final result and push appropriate notification
    if (!result.errors.empty()) {
        result.success = false;
    }

    if (result.success) {
        // Successful sync
        std::ostringstream msg;
        if (result.files_uploaded == 0 && result.files_downloaded == 0) {
            msg << "Everything up to date";
        } else {
            msg << "Synced " << result.files_uploaded << " uploaded, "
                << result.files_downloaded << " downloaded.";
        }
        push_notification(static_cast<int>(GUI::NotificationType::CloudSyncCompleted), msg.str());
    } else {
        // Sync failed
        std::string error_msg = "Cloud sync failed";
        if (!result.errors.empty()) {
            error_msg += ": " + result.errors[0];
        }
        push_notification(static_cast<int>(GUI::NotificationType::CloudSyncError), error_msg);
    }

    return result;
}

CloudSyncManager::SyncResult CloudSyncManager::sync_directory(const std::string &preset_type, SyncMode mode)
{
    // Similar to sync() but limited to specific preset type
    // Implementation simplified for now
    return sync(mode);
}

CloudSyncManager::SyncStatus CloudSyncManager::get_status() const
{
    SyncStatus status;
    status.is_syncing = m_is_syncing;
    status.last_sync_time = m_last_sync_time;
    status.last_sync_mode = SYNC_BIDIRECTIONAL; // Default mode
    status.pending_deletions = m_deleted_files.size();

    return status;
}

bool CloudSyncManager::force_upload_all()
{
    auto result = sync(SYNC_PUSH);
    return result.success;
}

bool CloudSyncManager::force_download_all()
{
    auto result = sync(SYNC_PULL);
    return result.success;
}

void CloudSyncManager::set_auto_sync_enabled(bool enabled)
{
    m_auto_sync_enabled = enabled;

    if (m_app_config) {
        m_app_config->set("cloud_sync", "auto_sync", enabled ? "1" : "0");
        m_app_config->save();
    }
}

bool CloudSyncManager::handle_first_sync_ui()
{
    // This must be called from the main UI thread
    FirstSyncInfo info = check_first_sync();

    if (info.state == NOT_FIRST_SYNC) {
        // Not first sync, nothing to do
        return false;
    }

    // Handle automatic cases first (no UI dialog needed)
    if (info.state == FIRST_SYNC_BOTH_EMPTY) {
        // Both empty, just mark as synced
        m_last_sync_time = std::time(nullptr);
        save_deletion_log();
        BOOST_LOG_TRIVIAL(info) << "First sync: Both local and remote are empty, marking as synced";
        return true;
    }

    if (info.state == FIRST_SYNC_LOCAL_ONLY) {
        // Only local has data, auto-upload
        BOOST_LOG_TRIVIAL(info) << "First sync: Only local has data, auto-uploading";
        std::string backup_path;
        create_backup("First sync - before upload", backup_path);

        std::thread([this]() {
            this->sync(SYNC_PUSH);
        }).detach();
        return true;
    }

    if (info.state == FIRST_SYNC_REMOTE_ONLY) {
        // Only remote has data, auto-download
        BOOST_LOG_TRIVIAL(info) << "First sync: Only remote has data, auto-downloading";
        std::string backup_path;
        create_backup("First sync - before download", backup_path);

        std::thread([this]() {
            this->sync(SYNC_PULL);
        }).detach();
        return true;
    }

    // Both have data - need user choice
    // This requires the GUI, so we need to include the dialog header
    // For now, let's return a function that needs to be called from GUI context
    return false; // Will be implemented in GUI layer
}

void CloudSyncManager::track_preset_deletion(const std::string &preset_type, const std::string &filename)
{
    if (!is_enabled()) {
        return;
    }

    // Build remote path for this preset file
    std::string remote_path = get_remote_path(preset_type, filename);

    // Add to deletion log
    add_to_deletion_log(remote_path);

    // Save the deletion log immediately
    save_deletion_log();

    BOOST_LOG_TRIVIAL(info) << "Tracked preset deletion for sync: " << remote_path;
    std::cerr << "[CloudSync] Tracked deletion: " << filename << " (type: " << preset_type << ")" << std::endl;
}

void CloudSyncManager::trigger_auto_sync()
{
    // Re-read auto-sync setting from config in case it changed
    if (m_app_config) {
        m_auto_sync_enabled = m_app_config->get_bool("cloud_sync", "auto_sync");
    }

    // Check if auto-sync is enabled and we're not already syncing
    if (!m_auto_sync_enabled) {
        BOOST_LOG_TRIVIAL(trace) << "CloudSyncManager: Auto-sync is disabled, skipping trigger";
        std::cerr << "CloudSyncManager: Auto-sync is disabled, skipping trigger" << std::endl;
        return;
    }

    if (m_is_syncing) {
        BOOST_LOG_TRIVIAL(debug) << "CloudSyncManager: Sync already in progress, skipping trigger";
        std::cerr << "CloudSyncManager: Sync already in progress, skipping trigger" << std::endl;
        return;
    }

    if (!is_enabled()) {
        BOOST_LOG_TRIVIAL(debug) << "CloudSyncManager: Cloud sync not configured, skipping trigger";
        std::cerr << "CloudSyncManager: Cloud sync not configured, skipping trigger" << std::endl;
        return;
    }

    // Check if this is first sync
    FirstSyncInfo info = check_first_sync();

    if (info.state != NOT_FIRST_SYNC) {
        BOOST_LOG_TRIVIAL(info) << "CloudSyncManager: First sync detected, handling with UI";
        // First sync handling will be done in GUI layer
        // For now, just handle auto cases here
        if (info.state == FIRST_SYNC_BOTH_EMPTY) {
            m_last_sync_time = std::time(nullptr);
            save_deletion_log();
            return;
        } else if (info.state == FIRST_SYNC_LOCAL_ONLY) {
            std::string backup_path;
            create_backup("First sync - before upload", backup_path);
            std::thread([this]() {
                this->sync(SYNC_PUSH);
            }).detach();
            return;
        } else if (info.state == FIRST_SYNC_REMOTE_ONLY) {
            std::string backup_path;
            create_backup("First sync - before download", backup_path);
            std::thread([this]() {
                this->sync(SYNC_PULL);
            }).detach();
            return;
        }
        // FIRST_SYNC_BOTH_EXIST - need user choice, skip for now
        // This will be handled by explicit sync button in preferences
        BOOST_LOG_TRIVIAL(info) << "CloudSyncManager: First sync with both data exists, waiting for manual sync";
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "CloudSyncManager: Triggering auto-sync in background";
    std::cerr << "CloudSyncManager: Triggering auto-sync in background" << std::endl;

    // Launch sync in background thread to avoid blocking UI
    std::thread([this]() {
        this->sync(SYNC_BIDIRECTIONAL);
    }).detach();
}

// Helper methods

bool CloudSyncManager::ensure_remote_structure()
{
    if (!m_provider) {
        return false;
    }

    // Create base directory
    wxString error;
    if (!m_provider->create_directory(REMOTE_BASE_PATH, [&](wxString err) { error = err; })) {
        BOOST_LOG_TRIVIAL(trace) << "Note: Base directory may already exist: " << error.ToStdString();
    }

    // Create subdirectories for each preset type
    for (const auto &type : get_preset_types()) {
        std::string path = REMOTE_BASE_PATH + "/" + type;
        if (!m_provider->create_directory(path, [&](wxString err) { error = err; })) {
            BOOST_LOG_TRIVIAL(trace) << "Note: Directory may already exist: " << path;
        }
    }

    return true;
}

bool CloudSyncManager::scan_local_files(std::vector<std::pair<std::string, boost::filesystem::path>> &files)
{
    boost::filesystem::path base_dir = boost::filesystem::path(data_dir());
    boost::filesystem::path presets_dir;

    // Check if presets are in data_dir/presets or directly in data_dir
    boost::filesystem::path with_presets_subdir = base_dir / "presets";
    if (boost::filesystem::exists(with_presets_subdir) && boost::filesystem::is_directory(with_presets_subdir)) {
        presets_dir = with_presets_subdir;
        BOOST_LOG_TRIVIAL(info) << "Using presets directory: " << presets_dir.string();
    } else {
        // Try without presets subdirectory (preset types directly in data_dir)
        presets_dir = base_dir;
        BOOST_LOG_TRIVIAL(info) << "Using data directory for presets: " << presets_dir.string();
    }

    for (const auto &type : get_preset_types()) {
        boost::filesystem::path type_dir = presets_dir / type;

        if (!boost::filesystem::exists(type_dir)) {
            BOOST_LOG_TRIVIAL(debug) << "Preset type directory does not exist: " << type_dir.string();
            continue;
        }

        for (boost::filesystem::directory_iterator it(type_dir); it != boost::filesystem::directory_iterator(); ++it) {
            if (boost::filesystem::is_regular_file(it->path()) && it->path().extension() == ".ini") {
                std::string filename = it->path().filename().string();

                // Filter out temporary files (pattern: name-timestamp-random.ini)
                // Example: "Vyper%20-%20ABS.ini-1760536505.840984-cAS8Esas.ini"
                if (filename.find("-") != std::string::npos) {
                    // Check if filename has pattern: .ini-<digits>.<digits>-<random>.ini
                    size_t second_dash = filename.rfind("-");
                    if (second_dash != std::string::npos) {
                        size_t first_dash = filename.rfind("-", second_dash - 1);
                        if (first_dash != std::string::npos) {
                            std::string middle_part = filename.substr(first_dash + 1, second_dash - first_dash - 1);
                            // Check if middle part looks like a timestamp (contains digits and dots)
                            bool has_digit = false;
                            bool has_dot = false;
                            for (char c : middle_part) {
                                if (std::isdigit(c)) has_digit = true;
                                if (c == '.') has_dot = true;
                            }
                            if (has_digit && has_dot && middle_part.length() > 10) {
                                BOOST_LOG_TRIVIAL(debug) << "Filtering out temporary file: " << filename;
                                continue; // Skip this temporary file
                            }
                        }
                    }
                }

                files.push_back(std::make_pair(type, it->path()));
            }
        }
    }

    return true;
}

bool CloudSyncManager::scan_remote_files(std::vector<CloudFile> &files)
{
    if (!m_provider) {
        return false;
    }

    for (const auto &type : get_preset_types()) {
        std::string remote_path = REMOTE_BASE_PATH + "/" + type;
        std::vector<CloudFile> type_files;

        if (m_provider->list_files(remote_path, type_files, [](wxString err) {
            BOOST_LOG_TRIVIAL(error) << "Failed to list remote files: " << err.ToStdString();
        })) {
            for (const auto &file : type_files) {
                if (!file.is_directory) {
                    files.push_back(file);
                }
            }
        }
    }

    return true;
}

bool CloudSyncManager::upload_file(const boost::filesystem::path &local_path, const std::string &remote_path)
{
    if (!m_provider) {
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Uploading: " << local_path.filename().string() << " -> " << remote_path;

    bool success = m_provider->upload_file(local_path, remote_path,
        [](Http::Progress progress, bool &cancel) {
            // Progress callback
        },
        [](wxString error) {
            BOOST_LOG_TRIVIAL(error) << "Upload error: " << error.ToStdString();
        });

    return success;
}

bool CloudSyncManager::download_file(const std::string &remote_path, const boost::filesystem::path &local_path)
{
    if (!m_provider) {
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Downloading: " << remote_path << " -> " << local_path.filename().string();

    // Ensure local directory exists
    boost::filesystem::create_directories(local_path.parent_path());

    bool success = m_provider->download_file(remote_path, local_path,
        [](Http::Progress progress, bool &cancel) {
            // Progress callback
        },
        [](wxString error) {
            BOOST_LOG_TRIVIAL(error) << "Download error: " << error.ToStdString();
        });

    return success;
}

CloudSyncManager::FileAction CloudSyncManager::determine_action(
    const boost::filesystem::path &local_path,
    const CloudFile &remote_file,
    bool exists_local,
    bool exists_remote)
{
    std::string filename = local_path.filename().string();

    // Case 1: File in deletion log
    if (exists_remote && is_in_deletion_log(remote_file.path)) {
        std::cerr << "[Sync] " << filename << ": DELETE_REMOTE (in deletion log)" << std::endl;
        return DELETE_REMOTE;
    }

    // Case 2: Only local exists
    if (exists_local && !exists_remote) {
        std::string local_hash = calculate_file_hash(local_path);
        std::cerr << "[Sync] " << filename << ": UPLOAD (remote doesn't exist, local_hash="
                 << local_hash.substr(0, 8) << "...)" << std::endl;
        return UPLOAD;
    }

    // Case 3: Only remote exists
    if (!exists_local && exists_remote) {
        // Download remote temporarily to calculate hash
        boost::filesystem::path temp_path = boost::filesystem::temp_directory_path() /
            ("cloudsync_" + std::to_string(std::time(nullptr)) + "_" + remote_file.name);

        std::string remote_hash;
        if (m_provider->download_file(remote_file.path, temp_path,
            [](Http::Progress progress, bool &cancel) {},
            [](wxString error) {})) {

            remote_hash = calculate_file_hash(temp_path);
            boost::filesystem::remove(temp_path);
        }

        std::cerr << "[Sync] " << filename << ": DOWNLOAD (local doesn't exist, remote_hash="
                 << (remote_hash.empty() ? "unknown" : remote_hash.substr(0, 8)) << "...)" << std::endl;
        return DOWNLOAD;
    }

    // Case 4: Both exist - compare content and timestamp
    if (exists_local && exists_remote) {
        // Calculate local hash
        std::string local_hash = calculate_file_hash(local_path);
        time_t local_mtime = boost::filesystem::last_write_time(local_path);

        // Download remote temporarily to calculate hash
        boost::filesystem::path temp_path = boost::filesystem::temp_directory_path() /
            ("cloudsync_" + std::to_string(std::time(nullptr)) + "_" + local_path.filename().string());

        std::string remote_hash;
        if (m_provider->download_file(remote_file.path, temp_path,
            [](Http::Progress progress, bool &cancel) {},
            [](wxString error) {})) {

            remote_hash = calculate_file_hash(temp_path);
            boost::filesystem::remove(temp_path);
        }

        // If hashes match, files are identical
        if (!remote_hash.empty() && local_hash == remote_hash) {
            std::cerr << "[Sync] " << filename << ": SKIP (files identical, hash="
                     << local_hash.substr(0, 8) << "...)" << std::endl;
            return SKIP;
        }

        // Hashes differ - use timestamp to decide
        char local_time_str[32], remote_time_str[32];
        std::strftime(local_time_str, sizeof(local_time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&local_mtime));
        std::strftime(remote_time_str, sizeof(remote_time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&remote_file.modified_time));

        if (local_mtime > remote_file.modified_time) {
            std::cerr << "[Sync] " << filename << ": UPLOAD (local newer: "
                     << local_time_str << " > " << remote_time_str
                     << ", local_hash=" << local_hash.substr(0, 8)
                     << "..., remote_hash=" << (remote_hash.empty() ? "unknown" : remote_hash.substr(0, 8)) << "...)" << std::endl;
            return UPLOAD;
        } else {
            std::cerr << "[Sync] " << filename << ": DOWNLOAD (remote newer: "
                     << remote_time_str << " >= " << local_time_str
                     << ", local_hash=" << local_hash.substr(0, 8)
                     << "..., remote_hash=" << (remote_hash.empty() ? "unknown" : remote_hash.substr(0, 8)) << "...)" << std::endl;
            return DOWNLOAD;
        }
    }

    return SKIP;
}

std::string CloudSyncManager::calculate_file_hash(const boost::filesystem::path &path)
{
    boost::nowide::ifstream file(path.string(), std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    MD5_CTX md5Context;
    MD5_Init(&md5Context);

    char buffer[8192];
    while (file.good()) {
        file.read(buffer, sizeof(buffer));
        MD5_Update(&md5Context, buffer, file.gcount());
    }

    unsigned char result[MD5_DIGEST_LENGTH];
    MD5_Final(result, &md5Context);

    std::ostringstream sout;
    sout << std::hex << std::setfill('0');
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) {
        sout << std::setw(2) << (int)result[i];
    }

    return sout.str();
}

std::string CloudSyncManager::get_remote_path(const std::string &preset_type, const std::string &filename)
{
    return REMOTE_BASE_PATH + "/" + preset_type + "/" + filename;
}

bool CloudSyncManager::load_deletion_log()
{
    boost::filesystem::path log_path = boost::filesystem::path(data_dir()) / DELETION_LOG_FILE;

    if (!boost::filesystem::exists(log_path)) {
        return true; // No deletion log yet, not an error
    }

    try {
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(log_path.string(), pt);

        m_deleted_files.clear();

        for (const auto &entry : pt.get_child("deleted_files")) {
            DeletedFile deleted;
            deleted.remote_path = entry.second.get<std::string>("remote_path");
            deleted.deleted_at = entry.second.get<time_t>("deleted_at");

            m_deleted_files.push_back(deleted);
        }

        BOOST_LOG_TRIVIAL(info) << "Loaded " << m_deleted_files.size() << " deletion log entries";
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load deletion log: " << e.what();
        return false;
    }
}

bool CloudSyncManager::save_deletion_log()
{
    boost::filesystem::path log_path = boost::filesystem::path(data_dir()) / DELETION_LOG_FILE;

    try {
        boost::property_tree::ptree pt;
        boost::property_tree::ptree deleted_node;

        for (const auto &deleted : m_deleted_files) {
            boost::property_tree::ptree entry_node;
            entry_node.put("remote_path", deleted.remote_path);
            entry_node.put("deleted_at", deleted.deleted_at);

            deleted_node.push_back(std::make_pair("", entry_node));
        }

        pt.add_child("deleted_files", deleted_node);
        boost::property_tree::write_json(log_path.string(), pt);

        BOOST_LOG_TRIVIAL(debug) << "Saved " << m_deleted_files.size() << " deletion log entries";
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to save deletion log: " << e.what();
        return false;
    }
}

void CloudSyncManager::add_to_deletion_log(const std::string &remote_path)
{
    // Check if already in log
    for (const auto &deleted : m_deleted_files) {
        if (deleted.remote_path == remote_path) {
            return; // Already logged
        }
    }

    DeletedFile deleted(remote_path, std::time(nullptr));
    m_deleted_files.push_back(deleted);

    BOOST_LOG_TRIVIAL(debug) << "Added to deletion log: " << remote_path;
}

void CloudSyncManager::cleanup_old_deletions()
{
    time_t now = std::time(nullptr);
    time_t cutoff = now - DELETION_EXPIRY_DAYS;

    auto it = m_deleted_files.begin();
    while (it != m_deleted_files.end()) {
        if (it->deleted_at < cutoff) {
            BOOST_LOG_TRIVIAL(debug) << "Removing expired deletion log entry: " << it->remote_path;
            it = m_deleted_files.erase(it);
        } else {
            ++it;
        }
    }
}

bool CloudSyncManager::is_in_deletion_log(const std::string &remote_path) const
{
    for (const auto &deleted : m_deleted_files) {
        if (deleted.remote_path == remote_path) {
            return true;
        }
    }
    return false;
}

std::vector<DeletedFile> CloudSyncManager::get_deleted_files() const
{
    return m_deleted_files;
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
        BOOST_LOG_TRIVIAL(error) << "Failed to push notification: " << e.what();
    }
}

} // namespace Slic3r
