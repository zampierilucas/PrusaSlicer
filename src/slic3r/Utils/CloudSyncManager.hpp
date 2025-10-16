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

    // Test connection to cloud storage
    bool test_connection(wxString &error_msg);

    // Perform full sync (bidirectional)
    enum SyncMode {
        SYNC_PULL,  // Download from cloud only
        SYNC_PUSH,  // Upload to cloud only
        SYNC_BIDIRECTIONAL  // Two-way sync with conflict detection
    };

    struct SyncResult {
        bool success;
        size_t files_uploaded;
        size_t files_downloaded;
        std::vector<std::string> errors;

        SyncResult() : success(true), files_uploaded(0), files_downloaded(0) {}
    };

    typedef std::function<void(const std::string& /* message */, int /* progress */)> SyncProgressFn;

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
        size_t local_file_count;
        size_t remote_file_count;
        time_t local_last_modified;
        time_t remote_last_modified;

        FirstSyncInfo() : state(NOT_FIRST_SYNC), local_file_count(0),
                         remote_file_count(0), local_last_modified(0),
                         remote_last_modified(0) {}
    };

    // Check if this is first sync and gather info
    FirstSyncInfo check_first_sync();

    // Create backup before first sync
    bool create_backup(const std::string &backup_reason, std::string &backup_path);

    // Perform sync operation
    SyncResult sync(SyncMode mode = SYNC_BIDIRECTIONAL, SyncProgressFn progress_fn = nullptr);

    // Sync specific directory (e.g., "print", "filament", "printer")
    SyncResult sync_directory(const std::string &preset_type, SyncMode mode = SYNC_BIDIRECTIONAL);

    // Get list of files marked for deletion
    std::vector<DeletedFile> get_deleted_files() const;

    // Get sync status
    struct SyncStatus {
        bool is_syncing;
        time_t last_sync_time;
        SyncMode last_sync_mode;
        size_t pending_deletions;

        SyncStatus() : is_syncing(false), last_sync_time(0), last_sync_mode(SYNC_BIDIRECTIONAL),
                      pending_deletions(0) {}
    };

    SyncStatus get_status() const;

    // Force upload of local changes
    bool force_upload_all();

    // Force download from cloud (overwrite local)
    bool force_download_all();

    // Enable/disable auto-sync
    void set_auto_sync_enabled(bool enabled);
    bool is_auto_sync_enabled() const { return m_auto_sync_enabled; }

    // Trigger async sync in background if enabled and not already syncing
    // If this is first sync, will show UI dialog for direction choice
    void trigger_auto_sync();

    // Track deletion of a preset file for cloud sync
    // This should be called when a preset file is deleted locally
    void track_preset_deletion(const std::string &preset_type, const std::string &filename);

    // Perform first sync with UI dialog for direction choice
    // Returns true if sync was initiated, false if cancelled or not needed
    bool handle_first_sync_ui();

    CloudSyncManager(const CloudSyncManager&) = delete;
    CloudSyncManager& operator=(const CloudSyncManager&) = delete;

private:
    CloudSyncManager();
    ~CloudSyncManager() = default;

    // Helper methods
    bool ensure_remote_structure();
    bool scan_local_files(std::vector<std::pair<std::string, boost::filesystem::path>> &files);
    bool scan_remote_files(std::vector<CloudFile> &files);

    // Determine what action to take for a file
    enum FileAction { UPLOAD, DOWNLOAD, SKIP, DELETE_REMOTE };
    FileAction determine_action(const boost::filesystem::path &local_path,
                               const CloudFile &remote_file,
                               bool exists_local, bool exists_remote);

    bool upload_file(const boost::filesystem::path &local_path, const std::string &remote_path);
    bool download_file(const std::string &remote_path, const boost::filesystem::path &local_path);

    std::string calculate_file_hash(const boost::filesystem::path &path);
    std::string get_remote_path(const std::string &preset_type, const std::string &filename);

    // Helper to get all supported preset types
    static const std::vector<std::string>& get_preset_types();

    // Helper to validate preset type
    static bool is_valid_preset_type(const std::string &type);

    // Deletion log management
    bool load_deletion_log();
    bool save_deletion_log();
    void add_to_deletion_log(const std::string &remote_path);
    void cleanup_old_deletions(); // Remove entries >30 days old
    bool is_in_deletion_log(const std::string &remote_path) const;

    // Notification helper
    void push_notification(int type, const std::string &message = "");

    // Members
    std::unique_ptr<CloudSyncProvider> m_provider;
    AppConfig *m_app_config;

    std::vector<DeletedFile> m_deleted_files;
    std::mutex m_mutex;

    bool m_is_syncing;
    bool m_auto_sync_enabled;
    time_t m_last_sync_time;
    time_t m_last_cleanup_time;

    static const std::string DELETION_LOG_FILE;
    static const std::string REMOTE_BASE_PATH;
    static const time_t DELETION_EXPIRY_DAYS;
};

} // namespace Slic3r

#endif // slic3r_CloudSyncManager_hpp_
