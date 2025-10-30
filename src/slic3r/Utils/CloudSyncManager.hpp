///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CloudSyncManager_hpp_
#define slic3r_CloudSyncManager_hpp_

#include "CloudSync.hpp"
#include <memory>
#include <mutex>
#include <boost/filesystem/path.hpp>

namespace Slic3r {

class AppConfig;
class PresetBundle;

// Singleton manager for cloud sync operations
class CloudSyncManager
{
public:
    static CloudSyncManager& instance();

    // Initialize with app config
    bool initialize(AppConfig *config);

    // Check if cloud sync is enabled and configured
    bool is_enabled() const;

    // Result of a single-file sync operation
    struct SyncFileResult {
        bool success;
        bool was_uploaded;      // true if file was uploaded, false if downloaded, irrelevant if not synced
        std::string error_msg;  // Empty if successful, contains error details if failed

        SyncFileResult() : success(true), was_uploaded(false) {}
    };

    // First sync detection and setup
    enum FirstSyncState {
        NOT_FIRST_SYNC,      // Already synced before
        FIRST_SYNC_BOTH_EMPTY,  // Both local and remote are empty
        FIRST_SYNC_LOCAL_ONLY,  // Only local has data
        FIRST_SYNC_REMOTE_ONLY, // Only remote has data
        FIRST_SYNC_BOTH_EXIST   // Both have data - needs user choice
    };

    struct FirstSyncInfo {
        FirstSyncState state;
        bool local_exists;
        bool remote_exists;
        time_t local_last_modified;
        time_t remote_last_modified;

        FirstSyncInfo() : state(NOT_FIRST_SYNC), local_exists(false),
                         remote_exists(false), local_last_modified(0),
                         remote_last_modified(0) {}
    };

    // Check if this is first sync and gather info
    FirstSyncInfo check_first_sync();

    // Create backup before first sync
    bool create_backup(const std::string &backup_reason, std::string &backup_path);

    // First sync user choice (for when both local and remote exist)
    enum FirstSyncChoice {
        FIRST_SYNC_AUTO,      // Use automatic conflict resolution
        FIRST_SYNC_UPLOAD,    // Force upload (user chose to upload)
        FIRST_SYNC_DOWNLOAD   // Force download (user chose to download)
    };

    // Perform sync operation for bundle config
    // Uses smart conflict detection to automatically decide:
    // - Only local exists → upload
    // - Only remote exists → download
    // - Both exist → compare hashes, then timestamps (newer wins)
    // - Identical → skip
    // first_sync_choice: Optional override for first sync when both exist
    SyncFileResult sync(FirstSyncChoice first_sync_choice = FIRST_SYNC_AUTO);

    // Enable/disable auto-sync
    void set_auto_sync_enabled(bool enabled);
    bool is_auto_sync_enabled() const { return m_auto_sync_enabled; }

    // Trigger async sync in background if enabled and not already syncing
    void trigger_auto_sync();

    // Trigger sync with first-sync dialog if needed (call from main/GUI thread)
    // This checks for first sync conflict and shows dialog before spawning background thread
    void trigger_sync_with_first_sync_check();

    // Mark that local presets have been modified (save/rename/delete)
    // This updates the last_local_modification timestamp so sync can detect changes
    void mark_local_presets_modified();

    CloudSyncManager(const CloudSyncManager&) = delete;
    CloudSyncManager& operator=(const CloudSyncManager&) = delete;

private:
    CloudSyncManager();
    ~CloudSyncManager() = default;

    // Helper methods
    bool ensure_remote_structure();
    bool get_local_bundle_path(boost::filesystem::path &local_path);
    bool get_remote_bundle_info(CloudFile &remote_file);

    // Determine what action to take for the bundle file
    enum FileAction { UPLOAD, DOWNLOAD, SKIP };
    FileAction determine_action(bool exists_local, bool exists_remote,
                               time_t local_mtime, time_t remote_mtime,
                               const std::string &local_hash, const std::string &remote_hash);

    bool upload_file(const boost::filesystem::path &local_path, const std::string &remote_path);
    bool download_file(const std::string &remote_path, const boost::filesystem::path &local_path);

    std::string calculate_file_hash(const boost::filesystem::path &path);

    // Get stored last local modification time from config
    // Updated automatically when bundle is generated or imported
    time_t get_last_local_modification_time();

    // Bundle generation and import
    bool generate_bundle_for_sync(boost::filesystem::path &bundle_path, std::string &error_msg);
    bool import_bundle_from_sync(const boost::filesystem::path &bundle_path, std::string &error_msg, time_t remote_mtime = 0);

    // First sync marker management (decoupled from sync logic)
    bool has_synced_before() const;
    void mark_first_sync_complete();

    // Notification helper
    void push_notification(int type, const std::string &message = "");

    // Members
    std::unique_ptr<CloudSyncWebDAV> m_webdav;
    AppConfig *m_app_config;

    std::mutex m_mutex;

    bool m_is_syncing;
    bool m_auto_sync_enabled;

    static const std::string REMOTE_BUNDLE_PATH;
};

} // namespace Slic3r

#endif // slic3r_CloudSyncManager_hpp_
